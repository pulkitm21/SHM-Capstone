from datetime import datetime, timedelta, timezone

from fastapi import Cookie, Depends, HTTPException, Response

from auth.auth_repository import (
    delete_expired_sessions,
    delete_session,
    get_session,
    get_user,
    touch_session,
)
from auth.auth_security import (
    SESSION_COOKIE_NAME,
    SESSION_COOKIE_SAMESITE,
    SESSION_COOKIE_SECURE,
    SESSION_IDLE_MINUTES,
)


def _utc_now() -> datetime:
    return datetime.now(timezone.utc)


def _clear_session_cookie(response: Response):
    response.delete_cookie(SESSION_COOKIE_NAME, path="/")


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


def get_current_user(
    response: Response,
    session_id: str | None = Cookie(default=None, alias=SESSION_COOKIE_NAME),
):
    now_dt = _utc_now()
    now_iso = now_dt.isoformat()

    delete_expired_sessions(now_iso)

    if not session_id:
        _clear_session_cookie(response)
        raise HTTPException(status_code=401, detail="Not authenticated")

    session = get_session(session_id)
    if not session:
        _clear_session_cookie(response)
        raise HTTPException(status_code=401, detail="Not authenticated")

    try:
        expires_dt = datetime.fromisoformat(session["expires_at"])
    except ValueError:
        delete_session(session_id)
        _clear_session_cookie(response)
        raise HTTPException(status_code=401, detail="Invalid session")

    if expires_dt <= now_dt:
        delete_session(session_id)
        _clear_session_cookie(response)
        raise HTTPException(status_code=401, detail="Session expired")

    db_user = get_user(session["username"])
    if not db_user:
        delete_session(session_id)
        _clear_session_cookie(response)
        raise HTTPException(status_code=401, detail="Session user not found")

    next_expires_at = (now_dt + timedelta(minutes=SESSION_IDLE_MINUTES)).isoformat()
    touch_session(session_id, last_seen_at=now_iso, expires_at=next_expires_at)
    _set_session_cookie(response, session_id)

    return {
        "username": db_user["username"],
        "role": db_user["role"],
        "last_login_at": db_user["last_login_at"],
        "session_id": session_id,
        "session_expires_at": next_expires_at,
    }


def require_admin(user=Depends(get_current_user)):
    if user["role"] != "admin":
        raise HTTPException(status_code=403, detail="Admin access required")

    return user
