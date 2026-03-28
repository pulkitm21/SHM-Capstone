from fastapi import Depends

# DEV BYPASS: always return a local hardcoded admin user.
def get_current_user():
    return {
        "username": "devadmin",
        "role": "admin",
        "last_login_at": None,
    }


# DEV BYPASS: admin-only checks always pass.
def require_admin(user=Depends(get_current_user)):
    return user