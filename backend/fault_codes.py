# Central mapping of ESP32 fault codes to backend fault metadata

from dataclasses import dataclass
from typing import Optional


@dataclass(frozen=True)
class FaultDefinition:
    description: str
    sensor_type: str
    fault_type: str
    severity: int       # 1=info, 2=warning, 3=critical
    fault_status: str   # "active" or "resolved"
    timestamp_source: str  # "accelerometer", "inclinometer", "temperature", "node"
    is_stateful: bool
    state_key: Optional[str]


FAULT_CODE_MAP: dict[int, FaultDefinition] = {
    1: FaultDefinition( "Ethernet link down", "ethernet", "ethernet_link", 3, "active", "node", True, "ethernet_link" ),
    2: FaultDefinition( "Ethernet link recovered", "ethernet", "ethernet_link", 1, "resolved", "node", True, "ethernet_link") ,
    3: FaultDefinition( "Ethernet no IP / timeout", "ethernet", "ethernet_timeout", 3, "active", "node", False, None),
    4: FaultDefinition( "Nodal MQTT disconnected", "mqtt", "mqtt_connection", 3, "active", "node", True, "mqtt_connection"),
    5: FaultDefinition( "Nodal MQTT reconnected", "mqtt", "mqtt_connection", 1, "resolved", "node", True, "mqtt_connection"),
    6: FaultDefinition( "Nodal MQTT publish failed", "mqtt", "mqtt_publish", 2, "active", "node", False, None),
    7: FaultDefinition( "ADXL355 sample dropped", "accelerometer", "sample_drop", 2, "active", "accelerometer", False, None),
    8: FaultDefinition( "SCL3300 sample dropped", "inclinometer", "sample_drop", 2, "active", "inclinometer", False, None),
    9: FaultDefinition( "ADT7420 sample dropped", "temperature", "sample_drop", 2, "active", "temperature", False, None),
    10: FaultDefinition( "Reboot attempt", "system", "reboot_attempt", 2, "active", "node", False, None),
    11: FaultDefinition( "Watchdog reset detected at boot", "system", "watchdog_reset", 3, "active", "node", False, None),
    12: FaultDefinition( "Power loss detected at boot", "system", "power_loss", 3, "active", "node", True, "power_loss"),
    13: FaultDefinition( "Power restored", "system", "power_loss", 1, "resolved", "node", True, "power_loss"),
    14: FaultDefinition( "ADXL355 init failed", "accelerometer", "init_failed", 3, "active", "accelerometer", False, None),
    15: FaultDefinition( "SCL3300 init failed", "inclinometer", "init_failed", 3, "active", "inclinometer", False, None),
    16: FaultDefinition( "ADT7420 init failed", "temperature", "init_failed", 3, "active", "temperature", False, None),
    17: FaultDefinition( "SPI bus error", "accelerometer", "spi_bus", 3, "active", "node", False, None),
    18: FaultDefinition( "I2C bus error", "system", "i2c_bus", 3, "active", "node", False, None),
}


def get_fault_definition(code: int) -> FaultDefinition:
    # Fallback for unmapped fault codes.
    return FAULT_CODE_MAP.get(
        int(code),
        FaultDefinition(
            description=f"Unknown fault code {code}",
            sensor_type="system",
            fault_type="unknown",
            severity=2,
            fault_status="active",
            timestamp_source="node",
            is_stateful=False,
            state_key=None,
        ),
    )