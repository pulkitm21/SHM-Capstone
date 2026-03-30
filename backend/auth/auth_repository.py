from auth.auth_db import get_connection
from auth.auth_security import AUTH_BOOTSTRAP_ADMIN_USERNAME, hash_password


HIDDEN_SYSTEM_USERS = (
    {AUTH_BOOTSTRAP_ADMIN_USERNAME} if AUTH_BOOTSTRAP_ADMIN_USERNAME else set()
)


def _row_to_user(row):
    if not row:
        return None

    return {
        "username": row["username"],
        "role": row["role"],
        "last_login_at": row["last_login_at"],
    }


def _row_to_session(row):
    if not row:
        return None

    return {
        "session_id": row["session_id"],
        "username": row["username"],
        "created_at": row["created_at"],
        "last_seen_at": row["last_seen_at"],
        "expires_at": row["expires_at"],
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


def any_admin_exists() -> bool:
    conn = get_connection()

    row = conn.execute(
        "SELECT 1 FROM users WHERE role = 'admin' LIMIT 1"
    ).fetchone()

    conn.close()
    return row is not None


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


def create_session(
    session_id: str,
    username: str,
    created_at: str,
    last_seen_at: str,
    expires_at: str,
):
    conn = get_connection()

    conn.execute(
        """
        INSERT INTO sessions (session_id, username, created_at, last_seen_at, expires_at)
        VALUES (?, ?, ?, ?, ?)
        """,
        (session_id, username, created_at, last_seen_at, expires_at),
    )

    conn.commit()
    conn.close()


def get_session(session_id: str):
    conn = get_connection()

    row = conn.execute(
        """
        SELECT session_id, username, created_at, last_seen_at, expires_at
        FROM sessions
        WHERE session_id = ?
        """,
        (session_id,),
    ).fetchone()

    conn.close()
    return _row_to_session(row)


def touch_session(session_id: str, last_seen_at: str, expires_at: str):
    conn = get_connection()

    conn.execute(
        "UPDATE sessions SET last_seen_at = ?, expires_at = ? WHERE session_id = ?",
        (last_seen_at, expires_at, session_id),
    )

    conn.commit()
    conn.close()


def delete_session(session_id: str):
    conn = get_connection()

    conn.execute(
        "DELETE FROM sessions WHERE session_id = ?",
        (session_id,),
    )

    conn.commit()
    conn.close()


def delete_sessions_for_user(username: str):
    conn = get_connection()

    conn.execute(
        "DELETE FROM sessions WHERE username = ?",
        (username,),
    )

    conn.commit()
    conn.close()


def delete_expired_sessions(now_iso: str):
    conn = get_connection()

    conn.execute(
        "DELETE FROM sessions WHERE expires_at <= ?",
        (now_iso,),
    )

    conn.commit()
    conn.close()
