from datetime import datetime, timedelta, timezone
import re
import sqlite3
from urllib.parse import unquote

from fastapi import APIRouter, Cookie, Depends, HTTPException, Response

from auth.auth_dependencies import get_current_user, require_admin
from auth.auth_models import (
    CreateUserRequest,
    CreateUserResponse,
    CurrentUserResponse,
    DeleteUserResponse,
    LoginRequest,
    LoginResponse,
    LogoutResponse,
    ResetUserPasswordRequest,
    ResetUserPasswordResponse,
    UpdateUserRoleRequest,
    UpdateUserRoleResponse,
    UsersListResponse,
)
from auth.auth_repository import (
    HIDDEN_SYSTEM_USERS,
    create_session,
    create_user,
    delete_expired_sessions,
    delete_session,
    delete_sessions_for_user,
    delete_user,
    get_user,
    list_users,
    update_last_login,
    update_user_password,
    update_user_role,
)
from auth.auth_security import (
    SESSION_COOKIE_NAME,
    SESSION_COOKIE_SAMESITE,
    SESSION_COOKIE_SECURE,
    SESSION_IDLE_MINUTES,
    generate_session_id,
    verify_password,
)

router = APIRouter()

USERNAME_PATTERN = re.compile(r"^[A-Za-z0-9._-]{3,32}$")
PASSWORD_RULE_MESSAGE = (
    "Password must be at least 8 characters and include uppercase, lowercase, "
    "number, and special character."
)


def _utc_now() -> datetime:
    return datetime.now(timezone.utc)


def _validate_username(username: str) -> str:
    normalized = username.strip()

    if not USERNAME_PATTERN.fullmatch(normalized):
        raise HTTPException(
            status_code=400,
            detail=(
                "Username must be 3-32 characters and use only letters, "
                "numbers, dot, underscore, or dash."
            ),
        )

    return normalized


def _validate_password(password: str) -> str:
    normalized = password.strip()

    is_strong = (
        len(normalized) >= 8
        and re.search(r"[a-z]", normalized)
        and re.search(r"[A-Z]", normalized)
        and re.search(r"\d", normalized)
        and re.search(r"[^A-Za-z0-9]", normalized)
    )

    if not is_strong:
        raise HTTPException(status_code=400, detail=PASSWORD_RULE_MESSAGE)

    return normalized


def _user_payload_from_record(record):
    return {
        "username": record["username"],
        "role": record["role"],
        "last_login_at": record["last_login_at"],
    }


def _set_session_cookie(response: Response, session_id: str):
    response.set_cookie(
        key=SESSION_COOKIE_NAME,
        value=session_id,
        httponly=True,
        samesite=SESSION_COOKIE_SAMESITE,
        secure=SESSION_COOKIE_SECURE,
        path="/",
        max_age=SESSION_IDLE_MINUTES * 60,
    )


def _clear_session_cookie(response: Response):
    response.delete_cookie(SESSION_COOKIE_NAME, path="/")


@router.post("/api/auth/login", response_model=LoginResponse)
def login(payload: LoginRequest, response: Response):
    username = payload.username.strip()
    password = payload.password.strip()

    user = get_user(username)

    if not user or not verify_password(password, user["password_hash"]):
        raise HTTPException(status_code=401, detail="Invalid credentials")

    now_dt = _utc_now()
    now_iso = now_dt.isoformat()
    session_id = generate_session_id()
    expires_at = (now_dt + timedelta(minutes=SESSION_IDLE_MINUTES)).isoformat()

    updated_user = update_last_login(
        username=username,
        iso_ts=now_iso,
    )

    delete_expired_sessions(now_iso)
    create_session(
        session_id=session_id,
        username=username,
        created_at=now_iso,
        last_seen_at=now_iso,
        expires_at=expires_at,
    )
    _set_session_cookie(response, session_id)

    return {
        "ok": True,
        "user": _user_payload_from_record(updated_user),
    }


@router.post("/api/auth/logout", response_model=LogoutResponse)
def logout(
    response: Response,
    session_id: str | None = Cookie(default=None, alias=SESSION_COOKIE_NAME),
):
    if session_id:
        delete_session(session_id)

    _clear_session_cookie(response)
    return {"ok": True}


@router.get("/api/auth/me", response_model=CurrentUserResponse)
def get_me(user=Depends(get_current_user)):
    return {
        "authenticated": True,
        "user": {
            "username": user["username"],
            "role": user["role"],
            "last_login_at": user["last_login_at"],
        },
    }


@router.get("/api/users", response_model=UsersListResponse)
def get_users(admin=Depends(require_admin)):
    return {"users": list_users()}


@router.post("/api/users", response_model=CreateUserResponse)
def create_user_route(payload: CreateUserRequest, admin=Depends(require_admin)):
    username = _validate_username(payload.username)
    password = _validate_password(payload.password)

    if username in HIDDEN_SYSTEM_USERS:
        raise HTTPException(status_code=403, detail="That username is reserved")

    try:
        create_user(username=username, password=password, role=payload.role)
    except sqlite3.IntegrityError:
        raise HTTPException(status_code=409, detail="Username already exists")

    created_user = get_user(username)

    return {
        "ok": True,
        "user": {
            "username": created_user["username"],
            "role": created_user["role"],
            "last_login_at": created_user["last_login_at"],
        },
    }


@router.put("/api/users/{username}/role", response_model=UpdateUserRoleResponse)
def update_user_role_route(
    username: str,
    payload: UpdateUserRoleRequest,
    admin=Depends(require_admin),
):
    decoded_username = unquote(username).strip()

    if decoded_username in HIDDEN_SYSTEM_USERS:
        raise HTTPException(status_code=403, detail="System user cannot be modified")

    existing = get_user(decoded_username)
    if not existing:
        raise HTTPException(status_code=404, detail="User not found")

    updated_user = update_user_role(decoded_username, payload.role)
    delete_sessions_for_user(decoded_username)

    return {
        "ok": True,
        "user": updated_user,
    }


@router.put("/api/users/{username}/password", response_model=ResetUserPasswordResponse)
def reset_user_password_route(
    username: str,
    payload: ResetUserPasswordRequest,
    admin=Depends(require_admin),
):
    decoded_username = unquote(username).strip()

    if decoded_username in HIDDEN_SYSTEM_USERS:
        raise HTTPException(status_code=403, detail="System user cannot be modified")

    existing = get_user(decoded_username)
    if not existing:
        raise HTTPException(status_code=404, detail="User not found")

    password = _validate_password(payload.password)
    update_user_password(decoded_username, password)
    delete_sessions_for_user(decoded_username)

    return {"ok": True}


@router.delete("/api/users/{username}", response_model=DeleteUserResponse)
def delete_user_route(username: str, admin=Depends(require_admin)):
    decoded_username = unquote(username).strip()

    if decoded_username in HIDDEN_SYSTEM_USERS:
        raise HTTPException(status_code=403, detail="System user cannot be deleted")

    if decoded_username == admin["username"]:
        raise HTTPException(status_code=400, detail="You cannot delete the current account")

    existing = get_user(decoded_username)
    if not existing:
        raise HTTPException(status_code=404, detail="User not found")

    delete_sessions_for_user(decoded_username)
    delete_user(decoded_username)

    return {"ok": True}
