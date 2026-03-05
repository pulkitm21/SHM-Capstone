import { useEffect, useMemo, useState } from "react";
import { useNavigate } from "react-router-dom";

import SensorStatus from "../../components/SensorStatus/SensorStatus";
import SensorInfoCard, { type SensorMeta } from "../../components/SensorInfo/SensorInfo";
import SensorConfigCard, { type SensorConfig } from "../../components/SensorConfig/SensorConfig";
import FaultLog from "../../components/FaultLog/Log";
import SensorFilters, { type SensorValue } from "../../components/SensorFilter/SensorFilter";
import SensorLineChart from "../../components/SensorPlot/SensorPlot";

import { getSettings, putSettings, getSensorData, type ApiResponse } from "../../services/api";

import "./Home.css";

// ---- SETTINGS CACHE (stores last known backend settings locally)
const SETTINGS_CACHE_KEY = "shm_settings_cache";

function loadCachedSettings():
  | { savedAt: string; meta: any; config: any }
  | null {
  try {
    const raw = localStorage.getItem(SETTINGS_CACHE_KEY);
    if (!raw) return null;
    return JSON.parse(raw);
  } catch {
    return null;
  }
}

function saveCachedSettings(metaByNode: any, configByNode: any) {
  try {
    localStorage.setItem(
      SETTINGS_CACHE_KEY,
      JSON.stringify({
        savedAt: new Date().toISOString(),
        meta: metaByNode,
        config: configByNode,
      })
    );
  } catch {
    // ignore storage errors
  }
}

// stores last known plot data per node/sensor/channel/timeframe
const PLOT_CACHE_KEY = "shm_plot_cache";

type PlotCacheRecord = {
  savedAt: string;
  data: ApiResponse;
};

function plotCacheKey(params: {
  node: number;
  sensor: SensorValue;
  minutes: number;
  channel: string;
}) {
  return `${params.node}|${params.sensor}|${params.minutes}|${params.channel}`;
}

function loadPlotCache(): Record<string, PlotCacheRecord> {
  try {
    const raw = localStorage.getItem(PLOT_CACHE_KEY);
    if (!raw) return {};
    const parsed = JSON.parse(raw);
    if (!parsed || typeof parsed !== "object") return {};
    return parsed as Record<string, PlotCacheRecord>;
  } catch {
    return {};
  }
}

function savePlotCacheEntry(key: string, data: ApiResponse) {
  try {
    const all = loadPlotCache();
    all[key] = { savedAt: new Date().toISOString(), data };
    localStorage.setItem(PLOT_CACHE_KEY, JSON.stringify(all));
  } catch {
    // ignore storage errors
  }
}

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

// Frontend fallback defaults. To be removed later when backend is fully implemented and stable.
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

// Utility: normalize object keys like {"1": ...} into {1: ...}
function normalizeNodeKeyedObject<T>(obj: any): Record<number, T> {
  const out: Record<number, T> = {};
  if (!obj || typeof obj !== "object") return out;

  for (const [k, v] of Object.entries(obj)) {
    const n = Number(k);
    if (Number.isFinite(n)) out[n] = v as T;
  }
  return out;
}

export default function Home() {
  const navigate = useNavigate();
  const isOnline = true;

  const [node, setNode] = useState<number>(1);
  const [sensor, setSensor] = useState<SensorValue>("accelerometer");
  const [timeframeMin, setTimeframeMin] = useState<number>(60);
  const [channel, setChannel] = useState<string>("all");

  // Keep this behavior: sensor change resets channel to "all"
  useEffect(() => setChannel("all"), [sensor]);

  const NODE_OPTIONS = useMemo(() => Array.from({ length: 5 }, (_, i) => i + 1), []);

  // INITIALIZE SETTINGS STATE FROM CACHE (if available)
  const [metaByNode, setMetaByNode] = useState<Record<number, Record<SensorValue, SensorMeta>>>(() => {
    const cached = loadCachedSettings();
    return cached?.meta ?? { 1: FALLBACK_META };
  });

  const [configByNode, setConfigByNode] = useState<Record<number, Record<SensorValue, SensorConfig>>>(() => {
    const cached = loadCachedSettings();
    return cached?.config ?? { 1: FALLBACK_CONFIG };
  });

  const [settingsStatus, setSettingsStatus] = useState<string>("");

  // Load settings once (global)
  useEffect(() => {
    async function loadSettings() {
      try {
        setSettingsStatus("Loading settings…");
        const json = await getSettings();

        // Expecting:
        // json.meta   = { "1": { accelerometer: {...}, ... }, "2": {...}, ... }
        // json.config = { "1": { accelerometer: {...}, ... }, "2": {...}, ... }
        const rawMetaByNode = normalizeNodeKeyedObject<Record<SensorValue, SensorMeta>>(json.meta);
        const rawConfigByNode = normalizeNodeKeyedObject<Record<SensorValue, SensorConfig>>(json.config);

        // Merge defaults per node/sensor so missing fields don't break UI
        const nextMetaByNode: Record<number, Record<SensorValue, SensorMeta>> = {};
        const nextConfigByNode: Record<number, Record<SensorValue, SensorConfig>> = {};

        const nodeIds = new Set<number>([
          ...Object.keys(rawMetaByNode).map(Number),
          ...Object.keys(rawConfigByNode).map(Number),
          1,
        ]);

        nodeIds.forEach((n) => {
          nextMetaByNode[n] = { ...FALLBACK_META, ...(rawMetaByNode[n] ?? {}) };
          nextConfigByNode[n] = { ...FALLBACK_CONFIG, ...(rawConfigByNode[n] ?? {}) };
        });

        //  UPDATE UI STATE + CACHE AFTER SUCCESSFUL BACKEND FETCH
        setMetaByNode(nextMetaByNode);
        setConfigByNode(nextConfigByNode);
        saveCachedSettings(nextMetaByNode, nextConfigByNode);

        setSettingsStatus("");
      } catch (e: any) {
        console.error(e);

        // FALLBACK TO CACHED SETTINGS IF BACKEND UNAVAILABLE
        const cached = loadCachedSettings();

        if (cached?.savedAt) {
          setSettingsStatus(
            `Backend unreachable — using cached settings (last synced: ${new Date(
              cached.savedAt
            ).toLocaleString()})`
          );
        } else {
          setSettingsStatus(`Settings load failed: ${e?.message ?? "Unknown error"}`);
        }
      }
    }

    loadSettings();
  }, []);

  async function saveAllSettings(
    nextMetaByNode: Record<number, Record<SensorValue, SensorMeta>>,
    nextConfigByNode: Record<number, Record<SensorValue, SensorConfig>>
  ) {
    try {
      setSettingsStatus("Saving…");
      await putSettings({ meta: nextMetaByNode, config: nextConfigByNode });

      // KEEP LOCAL CACHE IN SYNC AFTER SAVE
      saveCachedSettings(nextMetaByNode, nextConfigByNode);

      setSettingsStatus("");
    } catch (e: any) {
      console.error(e);
      setSettingsStatus(`Save failed: ${e?.message ?? "Unknown error"}`);
    }
  }

  function ensureNodeDefaults(n: number) {
    setMetaByNode((prev) => (prev[n] ? prev : { ...prev, [n]: FALLBACK_META }));
    setConfigByNode((prev) => (prev[n] ? prev : { ...prev, [n]: FALLBACK_CONFIG }));
  }

  // Ensure selected node always has defaults so UI updates cleanly
  useEffect(() => {
    ensureNodeDefaults(node);
  }, [node]);

  function handleSaveMeta(updated: SensorMeta) {
    const currentNodeMeta = metaByNode[node] ?? FALLBACK_META;
    const nextNodeMeta = { ...currentNodeMeta, [sensor]: updated };

    const nextMetaByNode = { ...metaByNode, [node]: nextNodeMeta };
    setMetaByNode(nextMetaByNode);

    saveAllSettings(nextMetaByNode, configByNode);
  }

  function handleSaveConfig(updated: SensorConfig) {
    const currentNodeConfig = configByNode[node] ?? FALLBACK_CONFIG;
    const nextNodeConfig = { ...currentNodeConfig, [sensor]: updated };

    const nextConfigByNode = { ...configByNode, [node]: nextNodeConfig };
    setConfigByNode(nextConfigByNode);

    saveAllSettings(metaByNode, nextConfigByNode);
  }

  const metaForNode = metaByNode[node] ?? FALLBACK_META;
  const configForNode = configByNode[node] ?? FALLBACK_CONFIG;

  const meta = metaForNode[sensor];
  const config = configForNode[sensor];

  // Plot state
  const [apiData, setApiData] = useState<ApiResponse | null>(null);
  const [plotStatus, setPlotStatus] = useState("Loading…");

  useEffect(() => {
    async function loadPlot() {
      const cacheKey = plotCacheKey({
        node,
        sensor,
        minutes: timeframeMin,
        channel,
      });

      // SHOW CACHED PLOT IMMEDIATELY (if available), then refresh from backend
      const cache = loadPlotCache();
      const cachedEntry = cache[cacheKey];

      if (cachedEntry?.data?.points?.length) {
        setApiData(cachedEntry.data);
        setPlotStatus(
          `Using cached plot (last synced: ${new Date(cachedEntry.savedAt).toLocaleString()})`
        );
      } else {
        setPlotStatus("Loading…");
        setApiData(null);
      }

      try {
        const endpoint = ENDPOINT_BY_SENSOR[sensor];
        const json = await getSensorData(endpoint, {
          node,
          minutes: timeframeMin,
          channel,
        });

        setApiData(json);
        setPlotStatus("Loaded");

        // SAVE FRESH PLOT TO CACHE
        savePlotCacheEntry(cacheKey, json);
      } catch (err: any) {
        console.error(err);

        // If we already showed cached data, keep it and just show an offline message
        if (cachedEntry?.data?.points?.length) {
          setPlotStatus(
            `Backend unreachable — showing cached plot (last synced: ${new Date(
              cachedEntry.savedAt
            ).toLocaleString()})`
          );
          return;
        }

        setApiData(null);
        setPlotStatus(`Error: ${err?.message ?? "Unknown error"}`);
      }
    }

    loadPlot();
  }, [node, sensor, channel, timeframeMin]);

  return (
    <div className="sc-page">
      <div className="top-selectors">
        <div className="control-box">
          <label className="control-label">Node</label>
          <select
            className="control-select"
            value={node}
            onChange={(e) => setNode(Number(e.target.value))}
          >
            {NODE_OPTIONS.map((n) => (
              <option key={n} value={n}>
                {n}
              </option>
            ))}
          </select>
        </div>

        <div className="control-box">
          <label className="control-label">Sensor</label>
          <select
            className="control-select"
            value={sensor}
            onChange={(e) => setSensor(e.target.value as SensorValue)}
          >
            {SENSOR_OPTIONS.map((opt) => (
              <option key={opt.value} value={opt.value}>
                {opt.label}
              </option>
            ))}
          </select>
        </div>
      </div>

      <SensorStatus isOnline={isOnline} />

      <div className="sc-top-cards">
        <div>
          <SensorInfoCard meta={meta} onSave={handleSaveMeta} />
          {settingsStatus && <p style={{ marginTop: 8, marginBottom: 0 }}>{settingsStatus}</p>}
        </div>

        <SensorConfigCard config={config} onSave={handleSaveConfig} />

        <div className="sc-card">
          <div className="sc-card-title">Fault Log</div>
          <div className="sc-card-body">
            <FaultLog />
          </div>
        </div>
      </div>

      <SensorFilters
        timeframeOptions={[...TIMEFRAME_OPTIONS]}
        channelOptionsBySensor={CHANNELS_BY_SENSOR}
        sensor={sensor}
        timeframeMin={timeframeMin}
        channel={channel}
        onTimeframeChange={setTimeframeMin}
        onChannelChange={setChannel}
        onExport={() => navigate("/export")}
      />

      <div className="sc-plot-card">
        <p style={{ margin: 0 }}>{plotStatus}</p>
        {apiData && (
          <SensorLineChart
            title={`${SENSOR_OPTIONS.find((s) => s.value === sensor)?.label ?? "Sensor"} (Node ${node})`}
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