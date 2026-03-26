import type { FaultRow, NodeRecord } from "../../services/api";
import type { SensorMeta } from "../../components/SensorInfo/SensorInfo";
import type { SensorConfig } from "../../components/SensorConfig/SensorConfig";
import type {
  AccelerometerPlotResponse,
  InclinometerPlotResponse,
  TemperaturePlotResponse,
} from "../../services/api";

export type SensorValue = "accelerometer" | "inclinometer" | "temperature";

export const UI_PREVIEW_MODE = true;

const NOW = Date.now();

function isoMinutesAgo(minutesAgo: number): string {
  return new Date(NOW - minutesAgo * 60 * 1000).toISOString();
}

function isoSecondsAgo(secondsAgo: number): string {
  return new Date(NOW - secondsAgo * 1000).toISOString();
}

function buildNodeLabel(nodeId: number, serial: string): string {
  return `Node ${nodeId} - ${serial}`;
}

export const PREVIEW_NODES: NodeRecord[] = [
  {
    node_id: 1,
    serial: "WT01-N01",
    label: buildNodeLabel(1, "WT01-N01"),
    first_seen: isoMinutesAgo(180),
    last_seen: isoSecondsAgo(8),
    online: true,
    x: 0.5,
    y: 0.18,
    position_zone: "Top",
  },
  {
    node_id: 2,
    serial: "WT01-N02",
    label: buildNodeLabel(2, "WT01-N02"),
    first_seen: isoMinutesAgo(160),
    last_seen: isoSecondsAgo(14),
    online: true,
    x: 0.5,
    y: 0.46,
    position_zone: "Middle",
  },
  {
    node_id: 3,
    serial: "WT01-N03",
    label: buildNodeLabel(3, "WT01-N03"),
    first_seen: isoMinutesAgo(220),
    last_seen: isoMinutesAgo(9),
    online: false,
    x: 0.5,
    y: 0.74,
    position_zone: "Bottom",
  },
];

const BASE_ACCEL_CONFIG: SensorConfig = {
  odr_index: 2,
  range: 1,
  hpf_corner: 0,
  desired_odr_index: 2,
  desired_range: 1,
  desired_hpf_corner: 0,
  applied_odr_index: null,
  applied_range: null,
  applied_hpf_corner: null,
  current_state: "configured",
  pending_seq: null,
  applied_seq: null,
  last_ack_at: null,
  sync_status: null,
  pending_control_cmd: null,
  pending_control_seq: null,
  last_control_cmd: null,
  last_control_seq: null,
  last_control_ack_at: null,
  control_status: null,
};

const BASE_INCLIN_CONFIG: SensorConfig = {
  ...BASE_ACCEL_CONFIG,
  current_state: "configured",
};

const BASE_TEMP_CONFIG: SensorConfig = {
  ...BASE_ACCEL_CONFIG,
  current_state: "configured",
};

export const PREVIEW_META_BY_NODE: Record<
  number,
  Record<SensorValue, SensorMeta>
> = {
  1: {
    accelerometer: {
      model: "ADXL355",
      serial: "ACCEL-001",
      installationDate: "2026-01-10",
      orientation: "+X +Y +Z",
    },
    inclinometer: {
      model: "SCL3300",
      serial: "INCL-001",
      installationDate: "2026-01-10",
      orientation: "Roll / Pitch / Yaw",
    },
    temperature: {
      model: "ADT7420",
      serial: "TEMP-001",
      installationDate: "2026-01-10",
      orientation: "N/A",
    },
  },
  2: {
    accelerometer: {
      model: "ADXL355",
      serial: "ACCEL-002",
      installationDate: "2026-01-10",
      orientation: "+X +Y +Z",
    },
    inclinometer: {
      model: "SCL3300",
      serial: "INCL-002",
      installationDate: "2026-01-10",
      orientation: "Roll / Pitch / Yaw",
    },
    temperature: {
      model: "ADT7420",
      serial: "TEMP-002",
      installationDate: "2026-01-10",

      orientation: "N/A",
    },
  },
  3: {
    accelerometer: {
      model: "ADXL355",
      serial: "ACCEL-003",
      installationDate: "2026-01-10",

      orientation: "+X +Y +Z",
    },
    inclinometer: {
      model: "SCL3300",
      serial: "INCL-003",
      installationDate: "2026-01-10",

      orientation: "Roll / Pitch / Yaw",
    },
    temperature: {
      model: "ADT7420",
      serial: "TEMP-003",
      installationDate: "2026-01-10",

      orientation: "N/A",
    },
  },
};

export const PREVIEW_CONFIG_BY_NODE: Record<
  number,
  Record<SensorValue, SensorConfig>
> = {
  1: {
    accelerometer: {
      ...BASE_ACCEL_CONFIG,
      current_state: "recording",
      odr_index: 2,
      range: 1,
      hpf_corner: 0,
      desired_odr_index: 2,
      desired_range: 1,
      desired_hpf_corner: 0,
    },
    inclinometer: {
      ...BASE_INCLIN_CONFIG,
    },
    temperature: {
      ...BASE_TEMP_CONFIG,
    },
  },
  2: {
    accelerometer: {
      ...BASE_ACCEL_CONFIG,
      current_state: "configured",
      odr_index: 1,
      range: 2,
      hpf_corner: 1,
      desired_odr_index: 1,
      desired_range: 2,
      desired_hpf_corner: 1,
    },
    inclinometer: {
      ...BASE_INCLIN_CONFIG,
    },
    temperature: {
      ...BASE_TEMP_CONFIG,
    },
  },
  3: {
    accelerometer: {
      ...BASE_ACCEL_CONFIG,
      current_state: "unknown",
      odr_index: 2,
      range: 1,
      hpf_corner: 0,
      desired_odr_index: 2,
      desired_range: 1,
      desired_hpf_corner: 0,
    },
    inclinometer: {
      ...BASE_INCLIN_CONFIG,
      current_state: "unknown",
    },
    temperature: {
      ...BASE_TEMP_CONFIG,
      current_state: "unknown",
    },
  },
};

export const PREVIEW_FAULTS_BY_SERIAL: Record<string, FaultRow[]> = {
  "WT01-N01": [],
  "WT01-N02": [
    {
      id: 201,
      ts: isoMinutesAgo(12),
      serial_number: "WT01-N02",
      sensor_type: "accelerometer",
      fault_type: "sample_drop",
      severity: 2,
      fault_status: "active",
      description: "Accelerometer sample gap detected during burst transmission.",
    },
  ],
  "WT01-N03": [
    {
      id: 301,
      ts: isoMinutesAgo(27),
      serial_number: "WT01-N03",
      sensor_type: "ethernet",
      fault_type: "link_down",
      severity: 3,
      fault_status: "active",
      description: "Ethernet link lost. Node is currently offline.",
    },
    {
      id: 302,
      ts: isoMinutesAgo(41),
      serial_number: "WT01-N03",
      sensor_type: "temperature",
      fault_type: "sensor_timeout",
      severity: 2,
      fault_status: "active",
      description: "Temperature polling timed out before node disconnect.",
    },
  ],
};

function buildAccelerometerPreview(): AccelerometerPlotResponse {
  const points = Array.from({ length: 180 }, (_, i) => {
    const minutesAgo = 180 - i;
    const ts = isoMinutesAgo(minutesAgo);

    const phase = i / 10;
    const drift = i / 900;

    return {
      ts,
      x: Number((0.02 * Math.sin(phase) + drift).toFixed(5)),
      y: Number((0.018 * Math.cos(phase * 1.2) - drift / 2).toFixed(5)),
      z: Number((1.0 + 0.012 * Math.sin(phase * 0.7)).toFixed(5)),
    };
  });

  return {
    sensor: "accelerometer",
    unit: "g",
    node: 1,
    points,
  };
}

function buildInclinometerPreview(): InclinometerPlotResponse {
  const points = Array.from({ length: 120 }, (_, i) => {
    const minutesAgo = 120 - i;
    const ts = isoMinutesAgo(minutesAgo);

    const phase = i / 8;

    return {
      ts,
      roll: Number((0.28 + 0.06 * Math.sin(phase)).toFixed(4)),
      pitch: Number((-0.12 + 0.05 * Math.cos(phase * 0.9)).toFixed(4)),
      yaw: Number((0.03 * Math.sin(phase * 0.5)).toFixed(4)),
    };
  });

  return {
    sensor: "inclinometer",
    unit: "deg",
    node: 1,
    points,
  };
}

function buildTemperaturePreview(): TemperaturePlotResponse {
  const points = Array.from({ length: 120 }, (_, i) => {
    const minutesAgo = 120 - i;
    const ts = isoMinutesAgo(minutesAgo);

    return {
      ts,
      value: Number((22.2 + 1.1 * Math.sin(i / 14)).toFixed(2)),
    };
  });

  return {
    sensor: "temperature",
    unit: "C",
    node: 1,
    points,
  };
}

export function buildPreviewPlotData(sensor: SensorValue) {
  switch (sensor) {
    case "accelerometer":
      return buildAccelerometerPreview();
    case "inclinometer":
      return buildInclinometerPreview();
    case "temperature":
    default:
      return buildTemperaturePreview();
  }
}