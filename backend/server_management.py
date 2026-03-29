from __future__ import annotations

import os
import shlex
import sqlite3
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Optional

import backend.mqtt_listener_control as mqtt_listener_control
from export_utils import SENSOR_DATA_DIR


# These defaults can be overridden with environment variables on the Pi.
BACKEND_SERVICE_NAME = os.getenv("SHM_BACKEND_SERVICE", "shm-backend")
MQTT_SERVICE_NAME = os.getenv("SHM_MQTT_SERVICE", "mosquitto")
VPN_INTERFACE_NAME = os.getenv("SHM_VPN_INTERFACE", "tun0")
VPN_RENEW_COMMAND = os.getenv("SHM_VPN_RENEW_COMMAND", "").strip()
VPN_CERT_PATH = os.getenv("SHM_VPN_CERT_PATH", "").strip()


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def build_action_response(
    *,
    action: str,
    status: str,
    ok: bool = True,
    message: Optional[str] = None,
    **extra: Any,
) -> Dict[str, Any]:
    payload: Dict[str, Any] = {
        "ok": ok,
        "action": action,
        "status": status,
        "time": utc_now_iso(),
    }

    if message:
        payload["message"] = message

    payload.update(extra)
    return payload


def run_system_command(command: list[str]) -> subprocess.CompletedProcess[str]:
    """
    Run a Pi-side maintenance command and capture stdout/stderr for diagnostics.

    These actions assume the service account has the required sudo/systemctl
    permissions configured on the Raspberry Pi.
    """
    return subprocess.run(
        command,
        check=True,
        capture_output=True,
        text=True,
    )


def is_interface_up(interface_name: str) -> bool:
    """
    Return whether the requested network interface is currently up.
    """
    try:
        result = subprocess.run(
            ["ip", "link", "show", interface_name],
            check=True,
            capture_output=True,
            text=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False

    return "state UP" in result.stdout or "UP," in result.stdout


def internet_reachable(timeout_seconds: float = 1.5) -> bool:
    """
    Lightweight outbound reachability check.
    """
    import socket

    try:
        conn = socket.create_connection(("8.8.8.8", 53), timeout=timeout_seconds)
        conn.close()
        return True
    except OSError:
        return False


def read_uptime_seconds() -> Optional[float]:
    """
    Read host uptime from /proc/uptime.
    """
    try:
        with open("/proc/uptime", "r", encoding="utf-8") as f:
            first_token = f.read().split()[0]
        return float(first_token)
    except (OSError, ValueError, IndexError):
        return None


def read_boot_time_iso() -> Optional[str]:
    """
    Derive the last boot time from uptime.
    """
    uptime_seconds = read_uptime_seconds()
    if uptime_seconds is None:
        return None

    boot_dt = datetime.now(timezone.utc).timestamp() - uptime_seconds
    return datetime.fromtimestamp(boot_dt, tz=timezone.utc).isoformat()


def get_server_status_payload(system_health: Dict[str, Any]) -> Dict[str, Any]:
    """
    Return the reduced server status payload used by the dashboard.
    """
    uptime_seconds = read_uptime_seconds()

    return {
        "backend_status": system_health.get("status", "OFFLINE"),
        "mqtt_connected": bool(system_health.get("mqtt")),
        "ssd_available": bool(system_health.get("ssd")),
        "fault_db_available": bool(system_health.get("fault_db")),
        "uptime_seconds": round(uptime_seconds, 2) if uptime_seconds is not None else None,
        "last_boot": read_boot_time_iso(),
        "time": utc_now_iso(),
    }


def get_vpn_cert_expiry_iso() -> Optional[str]:
    """
    Optional certificate expiry inspection.
    This is intentionally best-effort because not every deployment will expose
    the OpenVPN certificate path in the same location.
    """
    if not VPN_CERT_PATH:
        return None

    cert_path = Path(VPN_CERT_PATH)
    if not cert_path.exists() or not cert_path.is_file():
        return None

    try:
        result = subprocess.run(
            ["openssl", "x509", "-enddate", "-noout", "-in", str(cert_path)],
            check=True,
            capture_output=True,
            text=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None

    output = result.stdout.strip()
    # Example: notAfter=Apr 14 12:00:00 2026 GMT
    if "=" not in output:
        return None

    value = output.split("=", 1)[1].strip()
    try:
        expiry = datetime.strptime(value, "%b %d %H:%M:%S %Y %Z")
        return expiry.replace(tzinfo=timezone.utc).isoformat()
    except ValueError:
        return value


def get_server_network_payload() -> Dict[str, Any]:
    """
    Return the reduced network payload used by the dashboard.
    """
    return {
        "vpn_connected": is_interface_up(VPN_INTERFACE_NAME),
        "vpn_cert_expires_at": get_vpn_cert_expiry_iso(),
        "internet_reachable": internet_reachable(),
        "time": utc_now_iso(),
    }


def restart_backend_service() -> Dict[str, Any]:
    run_system_command(["sudo", "systemctl", "restart", BACKEND_SERVICE_NAME])
    return build_action_response(
        action="restart-backend",
        status="accepted",
        message=f"Backend service restart requested for {BACKEND_SERVICE_NAME}.",
        service=BACKEND_SERVICE_NAME,
    )


def restart_mqtt_service() -> Dict[str, Any]:
    run_system_command(["sudo", "systemctl", "restart", MQTT_SERVICE_NAME])
    return build_action_response(
        action="restart-mqtt",
        status="accepted",
        message=f"MQTT service restart requested for {MQTT_SERVICE_NAME}.",
        service=MQTT_SERVICE_NAME,
    )


def reboot_server() -> Dict[str, Any]:
    run_system_command(["sudo", "reboot"])
    return build_action_response(
        action="reboot-server",
        status="accepted",
        message="Server reboot requested.",
    )


def renew_vpn_certificate() -> Dict[str, Any]:
    """
    This endpoint is intentionally command-driven because VPN renewal steps vary
    across deployments. Configure SHM_VPN_RENEW_COMMAND on the Pi to match the
    real renewal script or command.
    """
    if not VPN_RENEW_COMMAND:
        raise RuntimeError(
            "VPN renewal command is not configured. Set SHM_VPN_RENEW_COMMAND first."
        )

    command = shlex.split(VPN_RENEW_COMMAND)
    if not command:
        raise RuntimeError("VPN renewal command is empty.")

    run_system_command(command)

    return build_action_response(
        action="renew-vpn-certificate",
        status="accepted",
        message="VPN certificate renewal command completed.",
    )



def clear_faults_db(faults_db_path: str | Path) -> Dict[str, Any]:
    """
    Delete all fault rows from the SQLite fault log database.
    """
    db_path = Path(faults_db_path)

    if not db_path.exists() or not db_path.is_file():
        return build_action_response(
            action="clear-faults",
            status="skipped",
            message="Fault database is not available.",
            deleted_rows=0,
            fault_db=str(db_path),
        )

    con: Optional[sqlite3.Connection] = None

    try:
        con = sqlite3.connect(str(db_path))
        cur = con.cursor()

        # Count existing rows before deleting them.
        cur.execute("SELECT COUNT(*) FROM faults")
        row = cur.fetchone()
        deleted_rows = int(row[0] or 0) if row else 0

        # Remove every stored fault row.
        cur.execute("DELETE FROM faults")

        # Reset autoincrement so new rows start cleanly.
        try:
            cur.execute("DELETE FROM sqlite_sequence WHERE name = ?", ("faults",))
        except sqlite3.Error:
            pass

        con.commit()

        print(f"[server] Cleared faults DB: removed {deleted_rows} row(s) from {db_path}")

        return build_action_response(
            action="clear-faults",
            status="completed",
            message=f"Cleared {deleted_rows} fault log entr{'y' if deleted_rows == 1 else 'ies' }.",
            deleted_rows=deleted_rows,
            fault_db=str(db_path),
        )
    finally:
        if con is not None:
            con.close()

def prune_sensor_data(older_than_days: int) -> Dict[str, Any]:
    """
    Delete historical raw sensor files older than the requested age.
    Only raw storage files are removed here.
    """
    if older_than_days < 1:
        raise ValueError("older_than_days must be at least 1.")

    if not SENSOR_DATA_DIR.exists() or not SENSOR_DATA_DIR.is_dir():
        return build_action_response(
            action="prune-data",
            status="skipped",
            message="Sensor data directory is not available.",
            deleted_files=0,
            bytes_freed=0,
            data_dir=str(SENSOR_DATA_DIR),
        )

    cutoff_ts = time.time() - (older_than_days * 86400)

    deleted_files = 0
    bytes_freed = 0

    for path in SENSOR_DATA_DIR.iterdir():
        if not path.is_file():
            continue

        # Keep pruning scoped to raw hourly storage files only.
        if not (path.name.endswith(".bin") or path.name.endswith(".bin.gz")):
            continue

        try:
            mtime = path.stat().st_mtime
            size = path.stat().st_size
        except OSError:
            continue

        if mtime >= cutoff_ts:
            continue

        try:
            path.unlink()
            deleted_files += 1
            bytes_freed += size
        except OSError:
            continue

    return build_action_response(
        action="prune-data",
        status="completed",
        message=f"Deleted {deleted_files} file(s) older than {older_than_days} day(s).",
        deleted_files=deleted_files,
        bytes_freed=bytes_freed,
        data_dir=str(SENSOR_DATA_DIR),
        older_than_days=older_than_days,
    )