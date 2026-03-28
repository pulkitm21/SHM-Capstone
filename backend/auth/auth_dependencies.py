from fastapi import Depends, HTTPException, Request

from auth.auth_repository import get_user


def get_current_user(request: Request):
    """Resolve the signed-in user from the cookie and re-check it against the DB."""
    username = request.cookies.get("user")

    if not username:
        raise HTTPException(status_code=401, detail="Not authenticated")

    user = get_user(username)
    if not user:
        raise HTTPException(status_code=401, detail="Not authenticated")

    return {
        "username": user["username"],
        "role": user["role"],
        "last_login_at": user["last_login_at"],
    }


def require_admin(user=Depends(get_current_user)):
    if user["role"] != "admin":
        raise HTTPException(status_code=403, detail="Admin only")

    return user
