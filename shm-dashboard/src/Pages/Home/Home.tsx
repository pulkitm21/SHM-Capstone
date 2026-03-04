import { useEffect, useState } from "react";
import { useNavigate } from "react-router-dom";

import SystemStatus from "../../components/SystemStatus/SystemStatus";
import SensorInfoCard, { type SensorMeta } from "../../components/SensorInfo/SensorInfo";
import SensorConfigCard, { type SensorConfig } from "../../components/SensorConfig/SensorConfig";
import FaultLog from "../../components/FaultLog/Log";
import SensorFilters, { type SensorValue } from "../../components/SensorFilter/SensorFilter";

import SensorLineChart from "../../components/SensorPlot/SensorPlot";

import { getSettings, putSettings, getSensorData, type ApiResponse } from "../../services/api";

import "./Home.css";

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

// Frontend fallback defaults (used until backend loads)
const FALLBACK_META: Record<SensorValue, SensorMeta> = {
  accelerometer: {
    model: "ADXL355",
    serial: "SN00023",
    installationDate: "2024-03-15",
    location: "Tower",
    orientation: "+X +Y +Z",
  },
  inclinometer: {
    model: "SCL3300",
    serial: "SN00110",
    installationDate: "2024-03-15",
    location: "Foundation",
    orientation: "Pitch/Roll",
  },
  temperature: {
    model: "ADT7420",
    serial: "SN00402",
    installationDate: "2024-03-15",
    location: "Tower",
    orientation: "N/A",
  },
};

const FALLBACK_CONFIG: Record<SensorValue, SensorConfig> = {
  accelerometer: {
    samplingRate: "400",
    measurementRange: "2g",
    lowPassFilter: "none",
    highPassFilter: "none",
  },
  inclinometer: {
    samplingRate: "200",
    measurementRange: "2g",
    lowPassFilter: "none",
    highPassFilter: "none",
  },
  temperature: {
    samplingRate: "100",
    measurementRange: "2g",
    lowPassFilter: "none",
    highPassFilter: "none",
  },
};


export default function Home() {
  const navigate = useNavigate();
  const isOnline = true;

  const [sensor, setSensor] = useState<SensorValue>("accelerometer");
  const [timeframeMin, setTimeframeMin] = useState<number>(60);
  const [channel, setChannel] = useState<string>("all");

  useEffect(() => setChannel("all"), [sensor]);

  // Settings state (now backend-driven)
  const [metaBySensor, setMetaBySensor] = useState<Record<SensorValue, SensorMeta>>(FALLBACK_META);
  const [configBySensor, setConfigBySensor] = useState<Record<SensorValue, SensorConfig>>(FALLBACK_CONFIG);
  const [settingsStatus, setSettingsStatus] = useState<string>("");

  // Load settings from backend once on mount
  useEffect(() => {
    async function loadSettings() {
      try {
        setSettingsStatus("Loading settings…");
        const json = await getSettings();

        setMetaBySensor((prev) => ({
          ...prev,
          ...(json.meta as Record<string, SensorMeta>),
        }));

        setConfigBySensor((prev) => ({
          ...prev,
          ...(json.config as Record<string, SensorConfig>),
        }));

        setSettingsStatus("");
      } catch (e: any) {
        console.error(e);
        setSettingsStatus(`Settings load failed: ${e?.message ?? "Unknown error"}`);
      }
    }

    loadSettings();
  }, []);

  async function saveSettings(
    nextMeta: Record<SensorValue, SensorMeta>,
    nextConfig: Record<SensorValue, SensorConfig>
  ) {
    try {
      setSettingsStatus("Saving…");
      await putSettings({ meta: nextMeta, config: nextConfig });
      setSettingsStatus("");
    } catch (e: any) {
      console.error(e);
      setSettingsStatus(`Save failed: ${e?.message ?? "Unknown error"}`);
    }
  }

  function handleSaveMeta(updated: SensorMeta) {
    const nextMeta = { ...metaBySensor, [sensor]: updated };
    setMetaBySensor(nextMeta);
    saveSettings(nextMeta, configBySensor);
  }

  function handleSaveConfig(updated: SensorConfig) {
    const nextConfig = { ...configBySensor, [sensor]: updated };
    setConfigBySensor(nextConfig);
    saveSettings(metaBySensor, nextConfig);
  }

  const meta = metaBySensor[sensor];
  const config = configBySensor[sensor];

  // Plot
  const [apiData, setApiData] = useState<ApiResponse | null>(null);
  const [plotStatus, setPlotStatus] = useState("Loading…");

  useEffect(() => {
    async function load() {
      try {
        setPlotStatus("Loading…");
        setApiData(null);

        const endpoint = ENDPOINT_BY_SENSOR[sensor];
        const json = await getSensorData(endpoint, { minutes: timeframeMin, channel });

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
      {settingsStatus && <p style={{ marginTop: 0 }}>{settingsStatus}</p>}

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
          sensorKey={apiData.sensor ?? sensor}
          unit={apiData.unit ?? ""}
          points={apiData.points.map((p) => ({ ts: p.t, value: p.v }))}
          height={420}
        />
      )}
      </div>
    </div>
  );
}
