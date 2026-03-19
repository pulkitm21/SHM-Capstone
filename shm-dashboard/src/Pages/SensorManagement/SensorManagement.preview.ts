import type { SensorMeta } from "../../components/SensorInfo/SensorInfo";
import type { SensorConfig } from "../../components/SensorConfig/SensorConfig";
import type { ApiResponse, FaultRow, NodeRecord } from "../../services/api";

export type SensorValue = "accelerometer" | "inclinometer" | "temperature";

// TESTCODE: Toggle this on for frontend-only UI testing without backend connectivity.
export const UI_PREVIEW_MODE = true;

// TESTCODE: Mock nodes shown while backend node loading is disabled.
export const PREVIEW_NODES: NodeRecord[] = [
  {
    node_id: 1,
    label: "Node 1",
    serial: "NODE001",
    online: true,
    first_seen: "2026-03-10T14:20:00Z",
    last_seen: "2026-03-18T21:02:00Z",
  },
  {
    node_id: 2,
    label: "Node 2",
    serial: "NODE002",
    online: false,
    first_seen: "2026-03-09T11:05:00Z",
    last_seen: "2026-03-18T18:35:00Z",
  },
  {
    node_id: 3,
    label: "Node 3",
    serial: "NODE003",
    online: true,
    first_seen: "2026-03-11T08:40:00Z",
    last_seen: "2026-03-18T20:58:00Z",
  },
];

// TESTCODE: Mock metadata shown while backend settings loading is disabled.
// UPDATED: Removed "location" field (now handled by NodeTable via position_zone)
export const PREVIEW_META_BY_NODE: Record<number, Record<SensorValue, SensorMeta>> = {
  1: {
    accelerometer: {
      model: "ADXL355",
      serial: "—",
      installationDate: "—",
      orientation: "+X +Y +Z",
    },
    inclinometer: {
      model: "SCL3300",
      serial: "—",
      installationDate: "—",
      orientation: "Pitch / Roll",
    },
    temperature: {
      model: "ADT7420",
      serial: "—",
      installationDate: "—",
      orientation: "N/A",
    },
  },
  2: {
    accelerometer: {
      model: "ADXL355",
      serial: "—",
      installationDate: "—",
      orientation: "+X +Y +Z",
    },
    inclinometer: {
      model: "SCL3300",
      serial: "—",
      installationDate: "—",
      orientation: "Pitch / Roll",
    },
    temperature: {
      model: "ADT7420",
      serial: "—",
      installationDate: "—",
      orientation: "N/A",
    },
  },
  3: {
    accelerometer: {
      model: "ADXL355",
      serial: "—",
      installationDate: "—",
      orientation: "+X +Y +Z",
    },
    inclinometer: {
      model: "SCL3300",
      serial: "—",
      installationDate: "—",
      orientation: "Pitch / Roll",
    },
    temperature: {
      model: "ADT7420",
      serial: "—",
      installationDate: "—",
      orientation: "N/A",
    },
  },
};

// TESTCODE: Mock configuration shown while backend config loading is disabled.
export const PREVIEW_CONFIG_BY_NODE: Record<number, Record<SensorValue, SensorConfig>> = {
  1: {
    accelerometer: {
      highPassFilterDesired: "on",
      highPassFilterApplied: "on",
      highPassFilterStatus: "synced",
      lastRequestId: "preview-req-001",
      lastAckAt: "2026-03-18T21:01:30Z",
    },
    inclinometer: {
      highPassFilterDesired: "none",
      highPassFilterApplied: "none",
      highPassFilterStatus: "synced",
      lastRequestId: "preview-req-002",
      lastAckAt: "2026-03-18T20:45:10Z",
    },
    temperature: {
      highPassFilterDesired: "none",
      highPassFilterApplied: "none",
      highPassFilterStatus: "synced",
      lastRequestId: "preview-req-003",
      lastAckAt: "2026-03-18T20:15:00Z",
    },
  },
  2: {
    accelerometer: {
      highPassFilterDesired: "on",
      highPassFilterApplied: null,
      highPassFilterStatus: "failed",
      lastRequestId: "preview-req-004",
      lastAckAt: null,
    },
    inclinometer: {
      highPassFilterDesired: "none",
      highPassFilterApplied: "none",
      highPassFilterStatus: "pending",
      lastRequestId: "preview-req-005",
      lastAckAt: null,
    },
    temperature: {
      highPassFilterDesired: "none",
      highPassFilterApplied: "none",
      highPassFilterStatus: "synced",
      lastRequestId: "preview-req-006",
      lastAckAt: "2026-03-18T18:20:00Z",
    },
  },
  3: {
    accelerometer: {
      highPassFilterDesired: "none",
      highPassFilterApplied: "none",
      highPassFilterStatus: "synced",
      lastRequestId: "preview-req-007",
      lastAckAt: "2026-03-18T20:56:00Z",
    },
    inclinometer: {
      highPassFilterDesired: "none",
      highPassFilterApplied: "none",
      highPassFilterStatus: "synced",
      lastRequestId: "preview-req-008",
      lastAckAt: "2026-03-18T20:40:00Z",
    },
    temperature: {
      highPassFilterDesired: "none",
      highPassFilterApplied: "none",
      highPassFilterStatus: "synced",
      lastRequestId: "preview-req-009",
      lastAckAt: "2026-03-18T20:30:00Z",
    },
  },
};

// TESTCODE: Mock faults shown while backend fault loading is disabled.
export const PREVIEW_FAULTS_BY_SERIAL: Record<string, FaultRow[]> = {
  NODE001: [
    {
      id: 1,
      ts: "2026-03-18T20:58:00Z",
      serial_number: "NODE001",
      sensor_type: "accelerometer",
      fault_type: "High vibration warning",
      severity: 2,
      fault_status: "active",
      description: "Elevated vibration detected on X axis.",
    } as FaultRow,
    {
      id: 2,
      ts: "2026-03-18T20:44:00Z",
      serial_number: "NODE001",
      sensor_type: "temperature",
      fault_type: "Temperature threshold exceeded",
      severity: 3,
      fault_status: "active",
      description: "Internal enclosure temperature exceeded limit.",
    } as FaultRow,
  ],
  NODE002: [
    {
      id: 3,
      ts: "2026-03-18T18:10:00Z",
      serial_number: "NODE002",
      sensor_type: "inclinometer",
      fault_type: "Tilt drift detected",
      severity: 2,
      fault_status: "active",
      description: "Pitch trend deviating from expected baseline.",
    } as FaultRow,
  ],
  NODE003: [],
};

// TESTCODE: Build chart data so the plot renders without backend responses.
export function buildPreviewPlotData(sensor: SensorValue): ApiResponse {
  const now = Date.now();

  const points = Array.from({ length: 40 }, (_, i) => {
    const t = new Date(now - (39 - i) * 60 * 1000).toISOString();

    if (sensor === "accelerometer") {
      return { t, v: 0.08 + Math.sin(i / 5) * 0.03 };
    }

    if (sensor === "inclinometer") {
      return { t, v: 0.9 + Math.sin(i / 6) * 0.15 };
    }

    return { t, v: 28 + Math.sin(i / 7) * 2.5 };
  });

  return {
    sensor,
    unit: sensor === "temperature" ? "°C" : sensor === "accelerometer" ? "g" : "deg",
    points,
  } as ApiResponse;
}