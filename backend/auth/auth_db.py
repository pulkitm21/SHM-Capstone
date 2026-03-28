import sqlite3
from pathlib import Path

# Store auth DB separate from SSD so backend can run without SSD.
DB_PATH = Path("/home/pi/auth.db")


def get_connection():
    """Create one SQLite connection configured for dict-like row access."""
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    """Create the users table and apply lightweight schema migrations."""
    conn = get_connection()

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS users (
            username TEXT PRIMARY KEY,
            password_hash TEXT NOT NULL,
            role TEXT NOT NULL
        )
        """
    )

    columns = {
        row["name"]
        for row in conn.execute("PRAGMA table_info(users)").fetchall()
    }

    # Track last successful sign-in so the Users page can show recent access.
    if "last_login_at" not in columns:
        conn.execute("ALTER TABLE users ADD COLUMN last_login_at TEXT")

    conn.commit()
    conn.close()
