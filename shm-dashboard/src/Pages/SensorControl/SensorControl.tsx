// src/pages/SensorControl/SensorControl.tsx
import { useEffect, useState } from "react";
import { useNavigate } from "react-router-dom";

import SystemStatus from "../../components/SystemStatus/SystemStatus";
import SensorInfoCard, { type SensorMeta } from "../../components/SensorInfo/SensorInfo";
import SensorConfigCard, { type SensorConfig } from "../../components/SensorConfig/SensorConfig";
import FaultLog from "../../components/FaultLog/Log";

import SensorFilters, { type SensorValue } from "../../components/SensorFilter/SensorFilter";

import SensorLineChart from "../../components/SensorPlot/SensorPlot";
import type { SensorPoint } from "../../components/SensorPlot/SensorPlot";

import "./SensorControl.css";

const API_BASE = import.meta.env.VITE_API_BASE_URL;

type ApiResponse = {
  sensor: string;
  unit: string;
  points: SensorPoint[];
};

const SENSOR_OPTIONS = [
  { label: "Accelerometer", value: "accelerometer" },
  { label: "Inclinometer", value: "inclinometer" },
  { label: "Temperature", value: "temperature" },
] as const;

const TIMEFRAME_OPTIONS = [
  { label: "1 hour", minutes: 60 },
  { label: "6 hours", minutes: 360 },
  { label: "12 hours", minutes: 720 },
  { label: "1 day", minutes: 1440 },
] as const;

const CHANNELS_BY_SENSOR: Record<string, { label: string; value: string }[]> = {
  accelerometer: [
    { label: "All", value: "all" },
    { label: "X", value: "x" },
    { label: "Y", value: "y" },
    { label: "Z", value: "z" },
  ],
  inclinometer: [
    { label: "All", value: "all" },
    { label: "Pitch", value: "pitch" },
    { label: "Roll", value: "roll" },
  ],
  temperature: [{ label: "All", value: "all" }],
};

const ENDPOINT_BY_SENSOR: Record<SensorValue, string> = {
  accelerometer: "/api/accel",
  inclinometer: "/api/inclinometer",
  temperature: "/api/temperature",
};

// ---- Default meta (fallback if nothing in localStorage) ----
const DEFAULT_META: Record<SensorValue, SensorMeta & { unit: string; plotKey: string }> = {
  accelerometer: {
    model: "ADXL355",
    serial: "SN00023",
    installationDate: "2024-03-15",
    location: "Tower",
    orientation: "+X +Y +Z",
    unit: "g",
    plotKey: "accelerometer",
  },
  inclinometer: {
    model: "SCL3300",
    serial: "SN00110",
    installationDate: "2024-03-15",
    location: "Foundation",
    orientation: "Pitch/Roll",
    unit: "deg",
    plotKey: "inclinometer",
  },
  temperature: {
    model: "ADT7420",
    serial: "SN00402",
    installationDate: "2024-03-15",
    location: "Tower",
    orientation: "N/A",
    unit: "°C",
    plotKey: "temperature",
  },
};

// ---- Default config (fallback if nothing in localStorage) ----
const DEFAULT_CONFIG: Record<SensorValue, SensorConfig> = {
  accelerometer: {
    samplingRate: "400",
    measurementRange: "2g",
    lowPassFilter: "100",
    highPassFilter: "none",
  },
  inclinometer: {
    samplingRate: "200",
    measurementRange: "2g",
    lowPassFilter: "50",
    highPassFilter: "none",
  },
  temperature: {
    samplingRate: "100",
    measurementRange: "2g",
    lowPassFilter: "none",
    highPassFilter: "none",
  },
};

// localStorage helpers
function lsKey(kind: "meta" | "config", sensor: SensorValue) {
  return `shm:${kind}:${sensor}`;
}
function loadFromLS<T>(key: string): T | null {
  try {
    const raw = localStorage.getItem(key);
    if (!raw) return null;
    return JSON.parse(raw) as T;
  } catch {
    return null;
  }
}
function saveToLS<T>(key: string, value: T) {
  localStorage.setItem(key, JSON.stringify(value));
}

function buildQuery(params: Record<string, string | number | undefined>) {
  const qs = new URLSearchParams();
  Object.entries(params).forEach(([k, v]) => {
    if (v === undefined) return;
    qs.set(k, String(v));
  });
  return qs.toString();
}

export default function SensorControl() {
  const navigate = useNavigate();

  // static for now
  const isOnline = true;

  const [sensor, setSensor] = useState<SensorValue>("accelerometer");

  // default timeframe = 1 hour
  const [timeframeMin, setTimeframeMin] = useState<number>(60);

  const [channel, setChannel] = useState<string>("all");

  // reset channel safely when sensor changes
  useEffect(() => {
    setChannel("all");
  }, [sensor]);

  // persisted meta per sensor
  const [metaBySensor, setMetaBySensor] = useState(() => {
    const init: typeof DEFAULT_META = { ...DEFAULT_META };
    (Object.keys(DEFAULT_META) as SensorValue[]).forEach((s) => {
      const saved = loadFromLS<typeof DEFAULT_META[SensorValue]>(lsKey("meta", s));
      if (saved) init[s] = { ...init[s], ...saved };
    });
    return init;
  });

  // persisted config per sensor
  const [configBySensor, setConfigBySensor] = useState(() => {
    const init: typeof DEFAULT_CONFIG = { ...DEFAULT_CONFIG };
    (Object.keys(DEFAULT_CONFIG) as SensorValue[]).forEach((s) => {
      const saved = loadFromLS<SensorConfig>(lsKey("config", s));
      if (saved) init[s] = { ...init[s], ...saved };
    });
    return init;
  });

  const meta = metaBySensor[sensor];
  const config = configBySensor[sensor];

  function handleSaveMeta(updated: SensorMeta) {
    setMetaBySensor((prev) => {
      const next = { ...prev, [sensor]: { ...prev[sensor], ...updated } };
      saveToLS(lsKey("meta", sensor), next[sensor]);
      return next;
    });
  }

  function handleSaveConfig(updated: SensorConfig) {
    setConfigBySensor((prev) => {
      const next = { ...prev, [sensor]: { ...prev[sensor], ...updated } };
      saveToLS(lsKey("config", sensor), next[sensor]);
      return next;
    });
  }

  // API-backed plot data (refetch when sensor/channel/timeframe changes)
  const [apiData, setApiData] = useState<ApiResponse | null>(null);
  const [plotStatus, setPlotStatus] = useState("Loading…");

  useEffect(() => {
    async function load() {
      try {
        setPlotStatus("Loading…");
        setApiData(null);

        const endpoint = ENDPOINT_BY_SENSOR[sensor];
        const query = buildQuery({ minutes: timeframeMin, channel });

        const url = `${API_BASE}${endpoint}?${query}`;
        const res = await fetch(url);

        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        const json = (await res.json()) as ApiResponse;

        setApiData(json);
        setPlotStatus("Loaded");
      } catch (err: any) {
        console.error(err);
        setApiData(null);
        setPlotStatus(`Error: ${err?.message ?? "Unknown error"}`);
      }
    }

    load();
  }, [sensor, channel, timeframeMin]);

  return (
    <div className="sc-page">
      <SystemStatus isOnline={isOnline} />

      <div className="sc-top-cards">
        <SensorInfoCard meta={meta} onSave={handleSaveMeta} />
        <SensorConfigCard config={config} onSave={handleSaveConfig} />
        <div className="sc-card">
          <div className="sc-card-title">Fault Log</div>
          <div className="sc-card-body">
            <FaultLog />
          </div>
        </div>
      </div>

      <SensorFilters
        sensorOptions={[...SENSOR_OPTIONS]}
        timeframeOptions={[...TIMEFRAME_OPTIONS]}
        channelOptionsBySensor={CHANNELS_BY_SENSOR}
        sensor={sensor}
        timeframeMin={timeframeMin}
        channel={channel}
        onSensorChange={setSensor}
        onTimeframeChange={setTimeframeMin}
        onChannelChange={setChannel}
        onExport={() => navigate("/export")}
      />

      <div className="sc-plot-card">
        <p style={{ margin: 0 }}>{plotStatus}</p>

        {apiData && (
          <SensorLineChart
            title={SENSOR_OPTIONS.find((s) => s.value === sensor)?.label ?? "Sensor"}
            sensorKey={apiData.sensor}
            unit={apiData.unit}
            points={apiData.points}
            height={420}
          />
        )}
      </div>
    </div>
  );
}
