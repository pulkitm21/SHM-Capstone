from datetime import datetime, timezone
from typing import Any

from fault_codes import get_fault_definition
from fault_logger import log_fault_events


def now_iso() -> str:
    # Build a UTC ISO timestamp for final fallbacks.
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def _first_accel_timestamp(packet: dict) -> str | None:
    # Return the first accelerometer timestamp from the packet.
    accel_samples = packet.get("a")
    if isinstance(accel_samples, list) and accel_samples:
        first_sample = accel_samples[0]
        if isinstance(first_sample, list) and first_sample:
            ts = first_sample[0]
            if ts is not None:
                return str(ts)
    return None


def _inclinometer_timestamp(packet: dict) -> str | None:
    # Return the inclinometer timestamp from the packet.
    incl = packet.get("i")
    if isinstance(incl, list) and incl:
        ts = incl[0]
        if ts is not None:
            return str(ts)
    return None


def _temperature_timestamp(packet: dict) -> str | None:
    # Return the temperature timestamp from the packet.
    temp = packet.get("T")
    if isinstance(temp, list) and temp:
        ts = temp[0]
        if ts is not None:
            return str(ts)
    return None


def _first_available_packet_timestamp(packet: dict) -> str | None:
    # Follow packet-order fallback: accel first, then inclinometer, then temperature.
    return (
        _first_accel_timestamp(packet)
        or _inclinometer_timestamp(packet)
        or _temperature_timestamp(packet)
    )


def _resolve_fault_timestamp(packet: dict, fault_code: int, receive_iso: str) -> str:
    """
    Resolve the display/log timestamp for one fault code.

    Rules:
    - accelerometer faults -> first accel timestamp
    - inclinometer faults  -> inclinometer timestamp
    - temperature faults   -> temperature timestamp
    - node/system faults   -> first available packet timestamp
    - if preferred source is missing, fall back to first available packet timestamp
    - final fallback -> backend receive time
    """
    fault_def = get_fault_definition(int(fault_code))

    preferred_ts: str | None = None

    if fault_def.sensor_type == "accelerometer":
        preferred_ts = _first_accel_timestamp(packet)
    elif fault_def.sensor_type == "inclinometer":
        preferred_ts = _inclinometer_timestamp(packet)
    elif fault_def.sensor_type == "temperature":
        preferred_ts = _temperature_timestamp(packet)
    else:
        preferred_ts = _first_available_packet_timestamp(packet)

    return preferred_ts or _first_available_packet_timestamp(packet) or receive_iso


def log_faults_from_packet(
    serial_number: str,
    packet: dict[str, Any],
    receive_iso: str | None = None,
) -> None:
    """
    Resolve timestamps for packet-level fault codes and forward them to the existing logger.

    This keeps fault timestamp logic outside delta_encoder.py while still reusing
    fault_logger.log_fault_events(), which accepts one timestamp per call.
    """
    fault_codes = packet.get("f", [])
    if not isinstance(fault_codes, list) or not fault_codes:
        return

    fallback_receive_iso = receive_iso or now_iso()

    # Group fault codes by their resolved timestamp so the existing logger can stay unchanged.
    codes_by_timestamp: dict[str, list[int]] = {}

    for raw_code in fault_codes:
        try:
            code = int(raw_code)
        except (TypeError, ValueError):
            print(f"[fault_event_ts] Skipping invalid fault code: {raw_code!r}")
            continue

        resolved_ts = _resolve_fault_timestamp(packet, code, fallback_receive_iso)
        codes_by_timestamp.setdefault(resolved_ts, []).append(code)

    for resolved_ts, grouped_codes in codes_by_timestamp.items():
        log_fault_events(
            serial_number=serial_number,
            fault_codes=grouped_codes,
            mqtt_ts=resolved_ts,
        )