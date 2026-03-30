import os
import secrets
from passlib.context import CryptContext

# Password hashing context.
pwd_context = CryptContext(schemes=["bcrypt"], deprecated="auto")

# Session cookie settings shared by routes and dependencies.
SESSION_COOKIE_NAME = "session_id"
SESSION_IDLE_MINUTES = int(os.getenv("AUTH_SESSION_IDLE_MINUTES", "5"))
SESSION_COOKIE_SAMESITE = os.getenv("AUTH_COOKIE_SAMESITE", "lax")
SESSION_COOKIE_SECURE = os.getenv("AUTH_COOKIE_SECURE", "false").lower() == "true"
AUTH_TEST_MODE = os.getenv("AUTH_TEST_MODE", "true").lower() == "true"


def hash_password(password: str) -> str:
    return pwd_context.hash(password)


def verify_password(password: str, hashed: str) -> bool:
    return pwd_context.verify(password, hashed)


def generate_session_id() -> str:
    return secrets.token_urlsafe(32)
