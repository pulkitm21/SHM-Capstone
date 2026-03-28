from typing import Literal
from pydantic import BaseModel

AuthRole = Literal["admin", "viewer"]


class LoginRequest(BaseModel):
    username: str
    password: str


class AuthUserResponse(BaseModel):
    username: str
    role: AuthRole
    last_login_at: str | None = None


class LoginResponse(BaseModel):
    ok: bool
    user: AuthUserResponse


class CurrentUserResponse(BaseModel):
    authenticated: bool
    user: AuthUserResponse | None


class LogoutResponse(BaseModel):
    ok: bool


class CreateUserRequest(BaseModel):
    username: str
    password: str
    role: AuthRole


class CreateUserResponse(BaseModel):
    ok: bool
    user: AuthUserResponse


class UsersListResponse(BaseModel):
    users: list[AuthUserResponse]


class UpdateUserRoleRequest(BaseModel):
    role: AuthRole


class UpdateUserRoleResponse(BaseModel):
    ok: bool
    user: AuthUserResponse


class ResetUserPasswordRequest(BaseModel):
    password: str


class ResetUserPasswordResponse(BaseModel):
    ok: bool


class DeleteUserResponse(BaseModel):
    ok: bool
