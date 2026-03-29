from fastapi import FastAPI, Query, Request, HTTPException, Depends
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import StreamingResponse
from datetime import datetime, timezone, timedelta
from typing import List, Dict, Any, Optional
from pydantic import BaseModel, Field
import asyncio
import json
import sqlite3
import shutil
import os
from pathlib import Path
from collections import deque
import subprocess
import math

import mqtt_listener_control as mqtt_listener_control
from mqtt_listener_control import start_listener

from sensor_health_cache import get_sensor_health_snapshot

from settings_schema import SettingsModel, to_dict, copy_deep

from settings_store import (
    load_settings,
    save_settings,
    ensure_node_defaults,
    # update_accelerometer_config_request,
    # mark_accelerometer_config_failed,
    get_site_name,
    update_site_name,
    # update_node_control_request,
    # mark_node_control_failed,
)

from node_registry import list_nodes, get_node_by_id, update_node_position
from mqtt_commands import publish_accelerometer_config, publish_node_control

from export_routes import router as export_router
from sensor_export_decoder import iter_decoded_records_for_export

from server_management import (
    clear_faults_db,
    get_server_network_payload,
    get_server_status_payload,
    prune_sensor_data,
    reboot_server as reboot_server_action,
    renew_vpn_certificate as renew_vpn_certificate_action,
    restart_backend_service as restart_backend_service_action,
    restart_mqtt_service as restart_mqtt_service_action,
)

from fault_logger import ensure_fault_db_schema

from auth.auth_routes import router as auth_router
from auth.auth_db import init_db
from auth.auth_repository import create_user, get_user
from auth.auth_dependencies import get_current_user, require_admin

app = FastAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=[
        "http://localhost:5173",
        "http://192.168.20.34:5173",
    ],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Protect export routes as authenticated read-only API routes.
app.include_router(export_router, dependencies=[Depends(get_current_user)])
app.include_router(auth_router)

DATA_DIR = Path("/mnt/ssd")

FAULT_DIR = DATA_DIR / "fault"
FAULTS_DB = FAULT_DIR / "faults.db"

# Sensor status values returned to the frontend.
SENSOR_STATUS_WINDOW_SECONDS = 120
SENSOR_KEYS = ("accelerometer", "inclinometer", "temperature")

# Stateful fault types that should be reduced to current state.
STATEFUL_FAULT_TYPES = (
    "ethernet_link",
    "mqtt_connection",
    "power_loss",
)


class NodePositionUpdate(BaseModel):
    x: float = Field(..., ge=0.0, le=1.0)
    y: float = Field(..., ge=0.0, le=1.0)


class AccelerometerConfigApplyRequest(BaseModel):
    odr_index: int = Field(..., ge=0, le=2)
    range: int = Field(..., ge=1, le=3)
    hpf_corner: int = Field(..., ge=0, le=6)


class NodeControlRequest(BaseModel):
    cmd: str = Field(..., pattern="^(start|stop|init|reset)$")


class SiteNameUpdate(BaseModel):
    site_name: str = Field(..., min_length=1, max_length=60)


class PruneStoredDataRequest(BaseModel):
    older_than_days: int = Field(..., ge=1)


def _iso_from_epoch_seconds(ts: float) -> str:
    return datetime.fromtimestamp(ts, tz=timezone.utc).isoformat()


def get_ssd_mount_status() -> Dict[str, Any]:
    """
    Diagnostic check for SSD mount health.
    Verifies whether /mnt/ssd exists, is mounted, and is accessible.
    """
    mount_path = DATA_DIR

    exists = mount_path.exists()
    mounted = os.path.ismount(mount_path)
    readable = os.access(mount_path, os.R_OK) if exists else False
    writable = os.access(mount_path, os.W_OK) if exists else False

    available = bool(exists and mounted and readable)

    return {
        "mount_path": str(mount_path),
        "exists": exists,
        "mounted": mounted,
        "readable": readable,
        "writable": writable,
        "available": available,
        "status": "mounted" if available else "unavailable",
        "time": datetime.now(timezone.utc).isoformat(),
    }


def _unmount_ssd():
    """
    Unmount the SSD mount path.
    """
    subprocess.run(["sudo", "umount", str(DATA_DIR)], check=True)


def is_ssd_available() -> bool:
    return bool(get_ssd_mount_status()["available"])


# Build one lightweight backend health snapshot for REST and SSE.
def get_system_health() -> Dict[str, Any]:
    ssd_status = get_ssd_mount_status()
    mqtt_ok = bool(mqtt_listener_control.MQTT_CONNECTED)
    ssd_ok = bool(ssd_status["available"])
    fault_db_ok = bool(ssd_ok and FAULTS_DB.exists())

    overall_ok = mqtt_ok and ssd_ok and fault_db_ok

    return {
        "status": "OK" if overall_ok else "DEGRADED",
        "mqtt": mqtt_ok,
        "ssd": ssd_ok,
        "fault_db": fault_db_ok,
        "time": datetime.now(timezone.utc).isoformat(),
    }


def empty_fault_response(page: int = 1, page_size: int = 15) -> Dict[str, Any]:
    safe_page = max(1, page)
    safe_page_size = max(1, page_size)

    return {
        "faults": [],
        "page": safe_page,
        "page_size": safe_page_size,
        "total_items": 0,
        "total_pages": 1,
        "filter_options": {
            "sensor_types": [],
            "fault_types": [],
            "severities": [],
            "statuses": [],
        },
    }


def _active_sensor_faults_for_serial(serial: str) -> dict[str, list[dict[str, Any]]]:
    """
    Return currently active stateful sensor faults grouped by sensor type for one node serial.
    Only accelerometer / inclinometer / temperature faults are included.
    """
    grouped: dict[str, list[dict[str, Any]]] = {
        "accelerometer": [],
        "inclinometer": [],
        "temperature": [],
    }

    if not is_ssd_available() or not FAULTS_DB.exists():
        return grouped

    con: Optional[sqlite3.Connection] = None

    try:
        con = sqlite3.connect(str(FAULTS_DB))
        con.row_factory = sqlite3.Row
        cur = con.cursor()

        # Reduce stateful fault history into one latest row per state key.
        cur.execute(
            """
            WITH normalized_faults AS (
                SELECT
                    id,
                    serial_number,
                    sensor_type,
                    fault_type,
                    COALESCE(state_key, fault_type) AS normalized_state_key,
                    COALESCE(
                        is_stateful,
                        CASE
                            WHEN fault_type IN ('ethernet_link', 'mqtt_connection', 'power_loss') THEN 1
                            ELSE 0
                        END
                    ) AS normalized_is_stateful,
                    severity,
                    fault_status,
                    description,
                    ts
                FROM faults
                WHERE serial_number = ?
            ),
            ranked_faults AS (
                SELECT
                    id,
                    serial_number,
                    sensor_type,
                    fault_type,
                    normalized_state_key AS state_key,
                    normalized_is_stateful AS is_stateful,
                    severity,
                    fault_status,
                    description,
                    ts,
                    ROW_NUMBER() OVER (
                        PARTITION BY serial_number, sensor_type, normalized_state_key
                        ORDER BY ts DESC, id DESC
                    ) AS rn
                FROM normalized_faults
                WHERE normalized_is_stateful = 1
            )
            SELECT
                id,
                serial_number,
                sensor_type,
                fault_type,
                state_key,
                is_stateful,
                severity,
                fault_status,
                description,
                ts
            FROM ranked_faults
            WHERE rn = 1
              AND LOWER(fault_status) = 'active'
              AND LOWER(sensor_type) IN ('accelerometer', 'inclinometer', 'temperature')
            ORDER BY ts DESC
            """,
            [serial],
        )

        for row in cur.fetchall():
            item = dict(row)
            sensor_type = str(item.get("sensor_type", "")).lower().strip()
            if sensor_type in grouped:
                grouped[sensor_type].append(item)

        return grouped

    except sqlite3.Error:
        return grouped

    finally:
        if con is not None:
            con.close()


def _recent_sensor_data_presence(
    serial: str,
    window_seconds: int = SENSOR_STATUS_WINDOW_SECONDS,
) -> dict[str, dict[str, Any]]:
    """
    Read recent per-sensor health from the live MQTT packet cache.

    A sensor counts as having recent data only when its cached timestamps
    are within the requested window.
    """
    now_dt = datetime.now(timezone.utc)

    status = {
        "accelerometer": {
            "has_data": False,
            "has_valid_data": False,
            "has_nan_data": False,
            "last_ts": None,
        },
        "inclinometer": {
            "has_data": False,
            "has_valid_data": False,
            "has_nan_data": False,
            "last_ts": None,
        },
        "temperature": {
            "has_data": False,
            "has_valid_data": False,
            "has_nan_data": False,
            "last_ts": None,
        },
    }

    live_health = get_sensor_health_snapshot(serial)

    def _parse_iso(ts: Any) -> Optional[datetime]:
        if not ts:
            return None
        try:
            return datetime.fromisoformat(str(ts).replace("Z", "+00:00")).astimezone(timezone.utc)
        except Exception:
            return None

    def _is_recent(ts: Any) -> bool:
        dt = _parse_iso(ts)
        if dt is None:
            return False
        return (now_dt - dt).total_seconds() <= window_seconds

    for sensor_name in SENSOR_KEYS:
        sensor_health = live_health.get(sensor_name, {})
        last_packet_ts = sensor_health.get("last_packet_ts")
        last_valid_ts = sensor_health.get("last_valid_data_ts")
        last_nan_ts = sensor_health.get("last_nan_ts")

        has_recent_packet = _is_recent(last_packet_ts)
        has_recent_valid = _is_recent(last_valid_ts)
        has_recent_nan = _is_recent(last_nan_ts)

        status[sensor_name]["has_data"] = has_recent_packet
        status[sensor_name]["has_valid_data"] = has_recent_valid
        status[sensor_name]["has_nan_data"] = has_recent_nan

        # Prefer the most recent packet timestamp for display.
        if has_recent_packet:
            status[sensor_name]["last_ts"] = last_packet_ts
        elif has_recent_valid:
            status[sensor_name]["last_ts"] = last_valid_ts
        elif has_recent_nan:
            status[sensor_name]["last_ts"] = last_nan_ts

    return status

def _sensor_health_state(
    *,
    node_online: bool,
    has_data: bool,
    has_valid_data: bool,
    has_nan_data: bool,
    active_faults: list[dict[str, Any]],
) -> str:
    """
    Sensor health priority:
    1. node offline -> offline
    2. recent NaN data -> offline
    3. active fault -> warning
    4. recent valid data -> online
    5. node online but no recent data -> idle
    6. fallback -> offline
    """
    if not node_online:
        return "offline"

    if has_nan_data:
        return "offline"

    if active_faults:
        return "warning"

    if has_valid_data:
        return "online"

    if not has_data:
        return "idle"

    return "offline"


def build_node_sensor_status(
    node_id: int,
    window_seconds: int = SENSOR_STATUS_WINDOW_SECONDS,
) -> Dict[str, Any]:
    """
    Build per-sensor health for one node.
    """
    node = get_node_by_id(node_id, timeout_seconds=60)
    if node is None:
        raise HTTPException(status_code=404, detail="Node not found")

    serial = str(node.get("serial") or "").strip()
    if not serial:
        raise HTTPException(status_code=400, detail="Node has no serial number")

    node_online = bool(node.get("online"))
    fault_groups = _active_sensor_faults_for_serial(serial)
    data_presence = _recent_sensor_data_presence(serial, window_seconds=window_seconds)

    sensors: dict[str, Any] = {}

    for sensor_name in SENSOR_KEYS:
        active_faults = fault_groups.get(sensor_name, [])
        sensor_presence = data_presence.get(sensor_name, {})
        has_data = bool(sensor_presence.get("has_data"))
        has_valid_data = bool(sensor_presence.get("has_valid_data"))
        has_nan_data = bool(sensor_presence.get("has_nan_data"))
        last_ts = sensor_presence.get("last_ts")

        sensors[sensor_name] = {
            "status": _sensor_health_state(
                node_online=node_online,
                has_data=has_data,
                has_valid_data=has_valid_data,
                has_nan_data=has_nan_data,
                active_faults=active_faults,
            ),
            "has_data": has_data,
            "has_valid_data": has_valid_data,
            "has_nan_data": has_nan_data,
            "last_data_ts": last_ts,
            "active_fault_count": len(active_faults),
            "active_faults": active_faults[:5],
        }

    return {
        "node_id": node["node_id"],
        "serial": serial,
        "node_online": node_online,
        "window_seconds": window_seconds,
        "sensors": sensors,
        "time": datetime.now(timezone.utc).isoformat(),
    }


@app.get("/")
def root():
    return {"message": "backend working"}


@app.get("/health")
def health():
    # Testing/manual health check endpoint only. SSE is used for backend status updates in the dashboard.
    return get_system_health()


@app.get("/api/events/health")
async def health_events(request: Request, user=Depends(get_current_user)):
    # SSE code for backend status live updates on the frontend dashboard.
    # This endpoint should remain independent of SSD availability.
    async def event_generator():
        try:
            while True:
                if await request.is_disconnected():
                    break

                payload = get_system_health()
                yield f"data: {json.dumps(payload)}\n\n"
                await asyncio.sleep(5)

        except asyncio.CancelledError:
            # Expected when the client disconnects or the backend shuts down.
            return

    return StreamingResponse(
        event_generator(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "Connection": "keep-alive",
        },
    )


@app.get("/api/storage")
def get_storage(user=Depends(get_current_user)):
    mount_status = get_ssd_mount_status()

    if not mount_status["available"]:
        return {
            "total_gb": 0,
            "used_gb": 0,
            "free_gb": 0,
            "usage_percent": 0,
            "ssd_status": mount_status,
        }

    try:
        total, used, free = shutil.disk_usage(DATA_DIR)
        return {
            "total_gb": round(total / (1024 ** 3), 2),
            "used_gb": round(used / (1024 ** 3), 2),
            "free_gb": round(free / (1024 ** 3), 2),
            "usage_percent": round((used / total) * 100, 2) if total > 0 else 0,
            "ssd_status": mount_status,
        }
    except OSError:
        return {
            "total_gb": 0,
            "used_gb": 0,
            "free_gb": 0,
            "usage_percent": 0,
            "ssd_status": {
                **mount_status,
                "available": False,
                "status": "unavailable",
            },
        }


@app.get("/api/storage/status")
def get_storage_status(user=Depends(get_current_user)):
    """
    Returns SSD mount diagnostic information for the dashboard.
    """
    return get_ssd_mount_status()


@app.post("/api/storage/unmount")
def unmount_storage(user=Depends(require_admin)):
    """
    Unmount the SSD safely.
    """
    mount_status = get_ssd_mount_status()

    if not mount_status["mounted"]:
        return {
            "ok": True,
            "action": "unmount",
            "status": "already_unmounted",
            "ssd_status": mount_status,
            "time": datetime.now(timezone.utc).isoformat(),
        }

    try:
        _unmount_ssd()
    except subprocess.CalledProcessError as exc:
        raise HTTPException(
            status_code=500,
            detail=f"Failed to unmount SSD: {exc}",
        )

    return {
        "ok": True,
        "action": "unmount",
        "status": "completed",
        "ssd_status": get_ssd_mount_status(),
        "time": datetime.now(timezone.utc).isoformat(),
    }


@app.get("/api/server/status")
def get_server_status(user=Depends(get_current_user)):
    """
    Detailed Pi/server status for the Server Management page.
    """
    return get_server_status_payload(get_system_health())


@app.get("/api/server/network")
def get_server_network(user=Depends(get_current_user)):
    """
    Connectivity details for VPN reachability and certificate health.
    """
    return get_server_network_payload()


@app.post("/api/server/restart-backend")
def restart_backend_service(user=Depends(require_admin)):
    """
    Restart the backend service running on the Raspberry Pi.
    """
    try:
        return restart_backend_service_action()
    except subprocess.CalledProcessError as exc:
        raise HTTPException(
            status_code=500,
            detail=f"Failed to restart backend service: {exc.stderr or exc.stdout or exc}",
        ) from exc
    except Exception as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc


@app.post("/api/server/restart-mqtt")
def restart_mqtt_service(user=Depends(require_admin)):
    """
    Restart the MQTT broker service on the Raspberry Pi.
    """
    try:
        return restart_mqtt_service_action()
    except subprocess.CalledProcessError as exc:
        raise HTTPException(
            status_code=500,
            detail=f"Failed to restart MQTT service: {exc.stderr or exc.stdout or exc}",
        ) from exc
    except Exception as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc


@app.post("/api/server/reboot")
def reboot_server(user=Depends(require_admin)):
    """
    Reboot the Raspberry Pi.
    """
    try:
        return reboot_server_action()
    except subprocess.CalledProcessError as exc:
        raise HTTPException(
            status_code=500,
            detail=f"Failed to reboot server: {exc.stderr or exc.stdout or exc}",
        ) from exc
    except Exception as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc


@app.post("/api/server/renew-vpn-certificate")
def renew_vpn_certificate(user=Depends(require_admin)):
    """
    Run the configured VPN certificate renewal command.
    """
    try:
        return renew_vpn_certificate_action()
    except subprocess.CalledProcessError as exc:
        raise HTTPException(
            status_code=500,
            detail=f"Failed to renew VPN certificate: {exc.stderr or exc.stdout or exc}",
        ) from exc
    except Exception as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc


@app.post("/api/server/prune-data")
def prune_data(payload: PruneStoredDataRequest, user=Depends(require_admin)):
    """
    Delete raw sensor data files older than the requested retention window.
    """
    try:
        return prune_sensor_data(payload.older_than_days)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except Exception as exc:
        raise HTTPException(
            status_code=500,
            detail=f"Failed to prune stored data: {exc}",
        ) from exc


@app.delete("/api/faults/clear")
def clear_faults(user=Depends(require_admin)):
    """
    Delete all stored fault log entries.
    """
    if not is_ssd_available():
        raise HTTPException(status_code=503, detail="SSD is unavailable.")

    try:
        return clear_faults_db(FAULTS_DB)
    except Exception as exc:
        raise HTTPException(
            status_code=500,
            detail=f"Failed to clear faults DB: {exc}",
        ) from exc


@app.get("/api/nodes")
def get_nodes(user=Depends(get_current_user)):
    return {"nodes": list_nodes(timeout_seconds=60)}


@app.get("/api/nodes/{node_id}")
def get_node(node_id: int, user=Depends(get_current_user)):
    node = get_node_by_id(node_id, timeout_seconds=60)
    if node is None:
        raise HTTPException(status_code=404, detail="Node not found")
    return {"node": node}


@app.get("/api/nodes/{node_id}/sensor-status")
def get_node_sensor_status(
    node_id: int,
    window_seconds: int = Query(default=SENSOR_STATUS_WINDOW_SECONDS, ge=30, le=3600),
    user=Depends(get_current_user),
):
    """
    Return per-sensor health for one node using recent data presence + active faults.
    """
    return build_node_sensor_status(node_id=node_id, window_seconds=window_seconds)


@app.put("/api/nodes/{node_id}/position")
def put_node_position(node_id: int, payload: NodePositionUpdate, user=Depends(require_admin)):
    updated = update_node_position(node_id=node_id, x=payload.x, y=payload.y)
    if updated is None:
        raise HTTPException(status_code=404, detail="Node not found")
    return {"ok": True, "node": updated}


# Start MQTT listener on app startup and store client reference for potential future use.
mqtt_status_client = None


@app.on_event("startup")
def startup_event():
    global mqtt_status_client

    # Initialize auth DB on backend startup.
    init_db()

    # Create the default local admin once.
    if not get_user("devadmin"):
        create_user("devadmin", "devadmin", "admin")
        print("[auth] Created default devadmin user")

    try:
        ensure_fault_db_schema()
    except Exception as e:
        print(f"[startup] Fault DB schema check failed: {e}")

    try:
        mqtt_status_client = start_listener()
    except Exception as e:
        mqtt_status_client = None
        print(f"[startup] MQTT listener not started: {e}")


@app.on_event("shutdown")
def shutdown_event():
    global mqtt_status_client

    if mqtt_status_client is None:
        return

    try:
        if hasattr(mqtt_status_client, "loop_stop"):
            mqtt_status_client.loop_stop()

        if hasattr(mqtt_status_client, "disconnect"):
            mqtt_status_client.disconnect()

    except Exception as e:
        print(f"[shutdown] MQTT listener cleanup failed: {e}")

    finally:
        mqtt_status_client = None


# =========================
# Bulk node position update endpoint
# Allows frontend to save all node positions in a single request
# Used for "Edit -> Move -> Save" workflow in NodeMap
# =========================
class BulkNodePositionItem(BaseModel):
    node_id: int
    x: float = Field(..., ge=0.0, le=1.0)
    y: float = Field(..., ge=0.0, le=1.0)


class BulkNodePositionUpdate(BaseModel):
    positions: List[BulkNodePositionItem]


@app.put("/api/nodes/positions")
def put_node_positions(payload: BulkNodePositionUpdate, user=Depends(require_admin)):
    updated_nodes = []

    for item in payload.positions:
        updated = update_node_position(
            node_id=item.node_id,
            x=item.x,
            y=item.y,
        )

        if updated is None:
            raise HTTPException(
                status_code=404,
                detail=f"Node {item.node_id} not found",
            )

        updated_nodes.append(updated)

    return {
        "ok": True,
        "nodes": updated_nodes,
    }


@app.get("/api/settings")
def get_settings(user=Depends(get_current_user)):
    for node in list_nodes(timeout_seconds=60):
        ensure_node_defaults(node["node_id"])

    settings = load_settings()
    return to_dict(settings)


@app.put("/api/settings")
def put_settings(payload: SettingsModel, user=Depends(require_admin)):
    merged = copy_deep(load_settings())

    merged.site_name = payload.site_name

    for node_id, per_sensor_meta in payload.meta.items():
        if node_id not in merged.meta:
            merged.meta[node_id] = {}
        merged.meta[node_id].update(per_sensor_meta)

    for node_id, per_sensor_cfg in payload.config.items():
        if node_id not in merged.config:
            merged.config[node_id] = {}
        merged.config[node_id].update(per_sensor_cfg)

    save_settings(merged)
    return {"ok": True, "settings": to_dict(merged)}


@app.get("/api/settings/site-name")
def api_get_site_name(user=Depends(get_current_user)):
    return {"site_name": get_site_name()}


@app.put("/api/settings/site-name")
def api_put_site_name(payload: SiteNameUpdate, user=Depends(require_admin)):
    updated_name = update_site_name(payload.site_name)
    return {"ok": True, "site_name": updated_name}


# Publish config immediately without seq/ack tracking.
# The older pending-sync workflow is intentionally commented out for now.
@app.post("/api/nodes/{node_id}/config/accelerometer/apply")
def apply_accelerometer_config(
    node_id: int,
    payload: AccelerometerConfigApplyRequest,
    user=Depends(require_admin),
):
    node = get_node_by_id(node_id, timeout_seconds=60)
    if node is None:
        raise HTTPException(status_code=404, detail="Node not found")

    ensure_node_defaults(node_id)

    try:
        publish_accelerometer_config(
            serial=node["serial"],
            odr_index=payload.odr_index,
            range_value=payload.range,
            hpf_corner=payload.hpf_corner,
        )
    except Exception as exc:
        raise HTTPException(
            status_code=503,
            detail=f"Failed to publish configure command: {exc}",
        )

    return {
        "ok": True,
        "node_id": node["node_id"],
        "serial": node["serial"],
        "sensor": "accelerometer",
        "desired": {
            "odr_index": payload.odr_index,
            "range": payload.range,
            "hpf_corner": payload.hpf_corner,
        },
        "status": "accepted",
    }


# Publish runtime control commands immediately without seq/ack tracking.
@app.post("/api/nodes/{node_id}/control")
def control_node(node_id: int, payload: NodeControlRequest, user=Depends(require_admin)):
    node = get_node_by_id(node_id, timeout_seconds=60)
    if node is None:
        raise HTTPException(status_code=404, detail="Node not found")

    ensure_node_defaults(node_id)

    try:
        publish_node_control(
            serial=node["serial"],
            cmd=payload.cmd,
        )
    except Exception as exc:
        raise HTTPException(
            status_code=503,
            detail=f"Failed to publish control command: {exc}",
        )

    return {
        "ok": True,
        "node_id": node["node_id"],
        "serial": node["serial"],
        "cmd": payload.cmd,
        "status": "accepted",
    }


def _normalize_fault_text(value: Optional[str]) -> str:
    return str(value or "").strip()


def _build_fault_where_clauses(
    serial_number: Optional[str] = None,
    sensor_type: Optional[str] = None,
    fault_type: Optional[str] = None,
    severity: Optional[int] = None,
    fault_status: Optional[str] = None,
    description: Optional[str] = None,
) -> tuple[list[str], list[Any]]:
    where_clauses: list[str] = []
    params: list[Any] = []

    serial_number_value = _normalize_fault_text(serial_number)
    sensor_type_value = _normalize_fault_text(sensor_type)
    fault_type_value = _normalize_fault_text(fault_type)
    fault_status_value = _normalize_fault_text(fault_status)
    description_value = _normalize_fault_text(description)

    if serial_number_value:
        where_clauses.append("serial_number LIKE ?")
        params.append(f"%{serial_number_value}%")

    if sensor_type_value:
        where_clauses.append("LOWER(sensor_type) = LOWER(?)")
        params.append(sensor_type_value)

    if fault_type_value:
        where_clauses.append("LOWER(fault_type) = LOWER(?)")
        params.append(fault_type_value)

    if severity is not None:
        where_clauses.append("severity = ?")
        params.append(severity)

    if fault_status_value:
        where_clauses.append("LOWER(fault_status) = LOWER(?)")
        params.append(fault_status_value)

    if description_value:
        where_clauses.append("description LIKE ?")
        params.append(f"%{description_value}%")

    return where_clauses, params


def _build_fault_where_sql(where_clauses: list[str]) -> str:
    if not where_clauses:
        return ""
    return "WHERE " + " AND ".join(where_clauses)


def _get_fault_filter_options(
    con: sqlite3.Connection,
    serial_number: Optional[str] = None,
) -> Dict[str, List[Any]]:
    serial_number_value = _normalize_fault_text(serial_number)

    if serial_number_value:
        serial_number_where_sql = "WHERE serial_number LIKE ?"
        serial_number_params: list[Any] = [f"%{serial_number_value}%"]
    else:
        serial_number_where_sql = ""
        serial_number_params = []

    cur = con.cursor()

    cur.execute(
        f"""
        SELECT DISTINCT sensor_type
        FROM faults
        {serial_number_where_sql}
        ORDER BY sensor_type COLLATE NOCASE ASC
        """,
        serial_number_params,
    )
    sensor_types = [row[0] for row in cur.fetchall() if row[0] not in (None, "")]

    cur.execute(
        f"""
        SELECT DISTINCT fault_type
        FROM faults
        {serial_number_where_sql}
        ORDER BY fault_type COLLATE NOCASE ASC
        """,
        serial_number_params,
    )
    fault_types = [row[0] for row in cur.fetchall() if row[0] not in (None, "")]

    cur.execute(
        f"""
        SELECT DISTINCT severity
        FROM faults
        {serial_number_where_sql}
        ORDER BY severity ASC
        """,
        serial_number_params,
    )
    severities = [row[0] for row in cur.fetchall() if row[0] is not None]

    cur.execute(
        f"""
        SELECT DISTINCT fault_status
        FROM faults
        {serial_number_where_sql}
        ORDER BY fault_status COLLATE NOCASE ASC
        """,
        serial_number_params,
    )
    statuses = [row[0] for row in cur.fetchall() if row[0] not in (None, "")]

    return {
        "sensor_types": sensor_types,
        "fault_types": fault_types,
        "severities": severities,
        "statuses": statuses,
    }


def read_fault_rows(
    serial_number: Optional[str],
    limit: int,
    page: int = 1,
    page_size: Optional[int] = None,
    sensor_type: Optional[str] = None,
    fault_type: Optional[str] = None,
    severity: Optional[int] = None,
    fault_status: Optional[str] = None,
    description: Optional[str] = None,
) -> Dict[str, Any]:
    effective_page_size = page_size if page_size is not None else limit

    if not is_ssd_available():
        return empty_fault_response(page=page, page_size=effective_page_size)

    if not FAULTS_DB.exists():
        return empty_fault_response(page=page, page_size=effective_page_size)

    safe_page = max(1, page)
    safe_page_size = max(1, effective_page_size)
    offset = (safe_page - 1) * safe_page_size

    con: Optional[sqlite3.Connection] = None

    try:
        con = sqlite3.connect(str(FAULTS_DB))
        con.row_factory = sqlite3.Row
        cur = con.cursor()

        where_clauses, where_params = _build_fault_where_clauses(
            serial_number=serial_number,
            sensor_type=sensor_type,
            fault_type=fault_type,
            severity=severity,
            fault_status=fault_status,
            description=description,
        )
        where_sql = _build_fault_where_sql(where_clauses)

        cur.execute(
            f"""
            SELECT COUNT(*) AS count
            FROM faults
            {where_sql}
            """,
            where_params,
        )
        total_items = int(cur.fetchone()["count"])
        total_pages = max(1, (total_items + safe_page_size - 1) // safe_page_size)

        cur.execute(
            f"""
            SELECT
                id,
                serial_number,
                fault_code,
                sensor_type,
                fault_type,
                state_key,
                is_stateful,
                severity,
                fault_status,
                description,
                ts
            FROM faults
            {where_sql}
            ORDER BY ts DESC
            LIMIT ? OFFSET ?
            """,
            [*where_params, safe_page_size, offset],
        )
        rows = [dict(r) for r in cur.fetchall()]

        filter_options = _get_fault_filter_options(con=con, serial_number=serial_number)

        return {
            "faults": rows,
            "page": safe_page,
            "page_size": safe_page_size,
            "total_items": total_items,
            "total_pages": total_pages,
            "filter_options": filter_options,
        }

    except sqlite3.Error as e:
        print(f"[faults] Failed to read faults DB: {e}")
        return empty_fault_response(page=safe_page, page_size=safe_page_size)

    finally:
        if con is not None:
            con.close()


def read_active_fault_summary() -> Dict[str, Any]:
    if not is_ssd_available() or not FAULTS_DB.exists():
        return {
            "by_serial": {},
            "warning_serials": [],
            "total_active_faults": 0,
            "time": datetime.now(timezone.utc).isoformat(),
        }

    con: Optional[sqlite3.Connection] = None

    try:
        con = sqlite3.connect(str(FAULTS_DB))
        con.row_factory = sqlite3.Row
        cur = con.cursor()

        # Reduce stateful fault history into one latest row per state key.
        cur.execute(
            """
            WITH normalized_faults AS (
                SELECT
                    id,
                    serial_number,
                    sensor_type,
                    fault_type,
                    COALESCE(state_key, fault_type) AS normalized_state_key,
                    COALESCE(
                        is_stateful,
                        CASE
                            WHEN fault_type IN ('ethernet_link', 'mqtt_connection', 'power_loss') THEN 1
                            ELSE 0
                        END
                    ) AS normalized_is_stateful,
                    fault_status,
                    ts
                FROM faults
            ),
            ranked_faults AS (
                SELECT
                    id,
                    serial_number,
                    sensor_type,
                    fault_type,
                    normalized_state_key AS state_key,
                    normalized_is_stateful AS is_stateful,
                    fault_status,
                    ts,
                    ROW_NUMBER() OVER (
                        PARTITION BY serial_number, sensor_type, normalized_state_key
                        ORDER BY ts DESC, id DESC
                    ) AS rn
                FROM normalized_faults
                WHERE normalized_is_stateful = 1
            )
            SELECT serial_number, COUNT(*) AS active_count, MAX(ts) AS latest_active_ts
            FROM ranked_faults
            WHERE rn = 1
              AND LOWER(fault_status) = 'active'
            GROUP BY serial_number
            ORDER BY serial_number COLLATE NOCASE ASC
            """
        )

        by_serial: Dict[str, Any] = {}
        total_active_faults = 0

        for row in cur.fetchall():
            serial = str(row["serial_number"] or "").strip()
            if not serial:
                continue

            active_count = int(row["active_count"] or 0)
            total_active_faults += active_count

            by_serial[serial] = {
                "active_count": active_count,
                "latest_active_ts": row["latest_active_ts"],
            }

        return {
            "by_serial": by_serial,
            "warning_serials": list(by_serial.keys()),
            "total_active_faults": total_active_faults,
            "time": datetime.now(timezone.utc).isoformat(),
        }

    except sqlite3.Error as e:
        print(f"[faults] Failed to build fault summary: {e}")
        return {
            "by_serial": {},
            "warning_serials": [],
            "total_active_faults": 0,
            "time": datetime.now(timezone.utc).isoformat(),
        }

    finally:
        if con is not None:
            con.close()


def read_fault_rows_since_id(
    after_id: int,
    limit: int,
    serial_number: Optional[str] = None,
) -> List[Dict[str, Any]]:
    if not is_ssd_available() or not FAULTS_DB.exists():
        return []

    con: Optional[sqlite3.Connection] = None

    try:
        con = sqlite3.connect(str(FAULTS_DB))
        con.row_factory = sqlite3.Row
        cur = con.cursor()

        where_clauses = ["id > ?"]
        params: List[Any] = [after_id]

        serial_number_value = _normalize_fault_text(serial_number)
        if serial_number_value:
            where_clauses.append("serial_number LIKE ?")
            params.append(f"%{serial_number_value}%")

        where_sql = "WHERE " + " AND ".join(where_clauses)

        cur.execute(
            f"""
            SELECT
                id,
                serial_number,
                fault_code,
                sensor_type,
                fault_type,
                state_key,
                is_stateful,
                severity,
                fault_status,
                description,
                ts
            FROM faults
            {where_sql}
            ORDER BY id ASC
            LIMIT ?
            """,
            [*params, limit],
        )

        return [dict(r) for r in cur.fetchall()]

    except sqlite3.Error as e:
        print(f"[faults] Failed to read fault delta rows: {e}")
        return []

    finally:
        if con is not None:
            con.close()


@app.get("/api/faults")
def get_faults(
    serial_number: Optional[str] = Query(default=None),
    sensor_type: Optional[str] = Query(default=None),
    fault_type: Optional[str] = Query(default=None),
    severity: Optional[int] = Query(default=None),
    fault_status: Optional[str] = Query(default=None),
    description: Optional[str] = Query(default=None),
    page: int = Query(default=1, ge=1),
    page_size: int = Query(default=15, ge=1, le=200),
    limit: int = Query(default=200, ge=1, le=5000),
    user=Depends(get_current_user),
):
    return read_fault_rows(
        serial_number=serial_number,
        sensor_type=sensor_type,
        fault_type=fault_type,
        severity=severity,
        fault_status=fault_status,
        description=description,
        page=page,
        page_size=page_size,
        limit=limit,
    )


@app.get("/api/faults/summary")
def get_fault_summary(user=Depends(get_current_user)):
    return read_active_fault_summary()


@app.get("/api/events/faults")
async def fault_events(
    request: Request,
    serial_number: Optional[str] = Query(default=None),
    limit: int = Query(default=200, ge=1, le=5000),
    user=Depends(get_current_user),
):
    # SSE code for live fault log updates on the frontend dashboard.
    # This now sends one initial snapshot, then only newly inserted fault rows.
    async def event_generator():
        last_seen_id = 0

        try:
            snapshot_rows = read_fault_rows(
                serial_number=serial_number,
                limit=limit,
            )["faults"]

            if snapshot_rows:
                last_seen_id = max(int(row.get("id") or 0) for row in snapshot_rows)

            snapshot_payload = {
                "mode": "snapshot",
                "faults": snapshot_rows,
                "last_id": last_seen_id,
                "time": datetime.now(timezone.utc).isoformat(),
            }

            yield f"data: {json.dumps(snapshot_payload)}\n\n"

            while True:
                if await request.is_disconnected():
                    break

                delta_rows = read_fault_rows_since_id(
                    after_id=last_seen_id,
                    limit=limit,
                    serial_number=serial_number,
                )

                if delta_rows:
                    last_seen_id = max(int(row.get("id") or 0) for row in delta_rows)

                    payload = {
                        "mode": "delta",
                        "faults": delta_rows,
                        "last_id": last_seen_id,
                        "time": datetime.now(timezone.utc).isoformat(),
                    }

                    yield f"data: {json.dumps(payload)}\n\n"

                await asyncio.sleep(5)

        except asyncio.CancelledError:
            return

    return StreamingResponse(
        event_generator(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "Connection": "keep-alive",
        },
    )


@app.get("/api/accel")
def get_accel_data(
    node: int = Query(1, ge=1),
    minutes: int = Query(1, ge=1, le=60),
    user=Depends(get_current_user),
):
    pts = read_accel_points(node_id=node, minutes=minutes)
    return {
        "sensor": "accelerometer",
        "unit": "g",
        "node": node,
        "points": pts,
    }


@app.get("/api/inclinometer")
def api_inclinometer(
    node: int = Query(1, ge=1),
    minutes: int = Query(10, ge=1, le=60),
    user=Depends(get_current_user),
):
    pts = read_inclinometer_points(node_id=node, minutes=minutes)
    return {
        "sensor": "inclinometer",
        "unit": "deg",
        "node": node,
        "points": pts,
    }


@app.get("/api/temperature")
def api_temperature(
    node: int = Query(1, ge=1),
    minutes: int = Query(60, ge=1, le=1440),
    user=Depends(get_current_user),
):
    pts = read_temperature_points(node_id=node, minutes=minutes)
    return {
        "sensor": "temperature",
        "unit": "C",
        "node": node,
        "points": pts,
    }


def _get_plot_node_serial(node_id: int) -> str:
    node = get_node_by_id(node_id, timeout_seconds=300)
    if node is None:
        raise HTTPException(status_code=404, detail="Node not found")

    serial = str(node.get("serial") or "").strip()
    if not serial:
        raise HTTPException(status_code=400, detail="Node has no serial number")

    return serial


def _plot_time_window(minutes: int) -> tuple[float, float, str, str]:
    end_dt = datetime.now().astimezone()
    start_dt = end_dt - timedelta(minutes=minutes)

    return (
        start_dt.timestamp(),
        end_dt.timestamp(),
        start_dt.isoformat(),
        end_dt.isoformat(),
    )


def _min_spacing_seconds(minutes: int, target_points: int) -> float:
    window_seconds = max(1, minutes * 60)
    return max(0.0, window_seconds / max(1, target_points))



def _plot_float_or_none(value):
    if value is None:
        return None
    return float(value)


def read_accel_points(node_id: int, minutes: int, limit: int = 1200):
    if not is_ssd_available():
        return []

    serial = _get_plot_node_serial(node_id)
    start_ts, end_ts, _, _ = _plot_time_window(minutes)

    hour_str = datetime.now().strftime("%Y%m%d_%H")
    file_path = DATA_DIR / "data" / f"data_{serial}_{hour_str}.bin"

    if not file_path.exists():
        return []

    points = deque(maxlen=limit)
    min_spacing = _min_spacing_seconds(minutes, limit)
    last_kept_ts = None

    for rec in iter_decoded_records_for_export(str(file_path)):
        accel_samples = rec.get("accel_samples")
        if not accel_samples:
            continue

        for ts, x, y, z in accel_samples:
            if ts < start_ts or ts >= end_ts:
                continue

            if last_kept_ts is not None and (ts - last_kept_ts) < min_spacing:
                continue

            points.append(
                {
                    "ts": _iso_from_epoch_seconds(ts),
                    "x": _plot_float_or_none(x),
                    "y": _plot_float_or_none(y),
                    "z": _plot_float_or_none(z),
                }
            )

            last_kept_ts = ts

    return list(points)


def read_inclinometer_points(node_id: int, minutes: int, limit: int = 1200):
    if not is_ssd_available():
        return []

    serial = _get_plot_node_serial(node_id)
    start_ts, end_ts, _, _ = _plot_time_window(minutes)

    hour_str = datetime.now().strftime("%Y%m%d_%H")
    file_path = DATA_DIR / "data" / f"data_{serial}_{hour_str}.bin"

    if not file_path.exists():
        return []

    points = deque(maxlen=limit)
    min_spacing = _min_spacing_seconds(minutes, limit)
    last_kept_ts = None

    for rec in iter_decoded_records_for_export(str(file_path)):
        inclin = rec.get("inclin")
        if not inclin:
            continue

        inclin_samples = inclin if isinstance(inclin, list) else [inclin]

        for ts, roll, pitch, yaw in inclin_samples:
            if ts < start_ts or ts >= end_ts:
                continue

            if last_kept_ts is not None and (ts - last_kept_ts) < min_spacing:
                continue

            points.append(
                {
                    "ts": _iso_from_epoch_seconds(ts),
                    "roll": _plot_float_or_none(roll),
                    "pitch": _plot_float_or_none(pitch),
                    "yaw": _plot_float_or_none(yaw),
                }
            )
            last_kept_ts = ts

    return list(points)


def read_temperature_points(node_id: int, minutes: int, limit: int = 2000):
    if not is_ssd_available():
        return []

    serial = _get_plot_node_serial(node_id)
    start_ts, end_ts, _, _ = _plot_time_window(minutes)

    hour_str = datetime.now().strftime("%Y%m%d_%H")
    file_path = DATA_DIR / "data" / f"data_{serial}_{hour_str}.bin"

    if not file_path.exists():
        return []

    points = deque(maxlen=limit)

    for rec in iter_decoded_records_for_export(str(file_path)):
        temp = rec.get("temp")
        if not temp:
            continue

        ts, value = temp
        if ts < start_ts or ts >= end_ts:
            continue

        points.append(
            {
                "ts": _iso_from_epoch_seconds(ts),
                "value": _plot_float_or_none(value),
            }
        )

    return list(points)
