import type { SensorMeta } from "../../components/SensorInfo/SensorInfo";
import type { ApiResponse, FaultRow, NodeRecord } from "../../services/api";
import type { SensorConfig } from "../../components/SensorConfig/SensorConfig";

export type SensorValue = "accelerometer" | "inclinometer" | "temperature";

export const UI_PREVIEW_MODE = false;

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

function baseConfig(overrides?: Partial<SensorConfig>): SensorConfig {
  return {
    odr_index: 2,
    range: 1,
    hpf_corner: 0,
    desired_odr_index: 2,
    desired_range: 1,
    desired_hpf_corner: 0,
    applied_odr_index: 2,
    applied_range: 1,
    applied_hpf_corner: 0,
    current_state: "configured",
    pending_seq: null,
    applied_seq: 12,
    last_ack_at: "2026-03-18T21:01:30Z",
    sync_status: "synced",
    ...overrides,
  };
}

export const PREVIEW_CONFIG_BY_NODE: Record<number, Record<SensorValue, SensorConfig>> = {
  1: {
    accelerometer: baseConfig({
      desired_odr_index: 2,
      desired_range: 1,
      desired_hpf_corner: 0,
      applied_odr_index: 2,
      applied_range: 1,
      applied_hpf_corner: 0,
      sync_status: "synced",
      current_state: "configured",
      applied_seq: 18,
      pending_seq: null,
      last_ack_at: "2026-03-18T21:01:30Z",
    }),
    inclinometer: baseConfig(),
    temperature: baseConfig(),
  },
  2: {
    accelerometer: baseConfig({
      desired_odr_index: 0,
      desired_range: 3,
      desired_hpf_corner: 4,
      applied_odr_index: 2,
      applied_range: 1,
      applied_hpf_corner: 0,
      sync_status: "pending",
      current_state: "reconfig",
      pending_seq: 27,
      applied_seq: 21,
      last_ack_at: "2026-03-18T18:05:00Z",
    }),
    inclinometer: baseConfig(),
    temperature: baseConfig(),
  },
  3: {
    accelerometer: baseConfig({
      desired_odr_index: 1,
      desired_range: 2,
      desired_hpf_corner: 2,
      applied_odr_index: 1,
      applied_range: 2,
      applied_hpf_corner: 2,
      sync_status: "synced",
      current_state: "recording",
      pending_seq: null,
      applied_seq: 31,
      last_ack_at: "2026-03-18T20:56:00Z",
    }),
    inclinometer: baseConfig(),
    temperature: baseConfig(),
  },
};

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