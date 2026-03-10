import { useEffect, useMemo, useState } from "react";
import { useNavigate, useSearchParams } from "react-router-dom";

import SensorStatus from "../../components/SensorStatus/SensorStatus";
import SensorInfoCard, { type SensorMeta } from "../../components/SensorInfo/SensorInfo";
import SensorConfigCard, { type SensorConfig } from "../../components/SensorConfig/SensorConfig";
import FaultLog from "../../components/FaultLog/Log";
import SensorFilters, { type SensorValue } from "../../components/SensorFilter/SensorFilter";
import SensorLineChart from "../../components/SensorPlot/SensorPlot";

import {
  getSettings,
  putSettings,
  getSensorData,
  getNodes,
  type ApiResponse,
  type NodeRecord,
} from "../../services/api";

import "./SensorManagement.css";

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
  } catch {}
}

const PLOT_CACHE_KEY = "shm_plot_cache";

type PlotCacheRecord = {
  savedAt: string;
  data: ApiResponse;
};

function plotCacheKey(params: {
  nodeKey: string;
  sensor: SensorValue;
  minutes: number;
  channel: string;
}) {
  return `${params.nodeKey}|${params.sensor}|${params.minutes}|${params.channel}`;
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
  } catch {}
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

const FALLBACK_META: Record<SensorValue, SensorMeta> = {
  accelerometer: {
    model: "ADXL355",
    serial: "ACCEL-001",
    installationDate: "2024-03-15",
    location: "Tower",
    orientation: "+X +Y +Z",
  },
  inclinometer: {
    model: "SCL3300",
    serial: "INCL-001",
    installationDate: "2024-03-15",
    location: "Foundation",
    orientation: "Pitch/Roll",
  },
  temperature: {
    model: "ADT7420",
    serial: "TEMP-001",
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

function normalizeNodeKeyedObject<T>(obj: any): Record<number, T> {
  const out: Record<number, T> = {};
  if (!obj || typeof obj !== "object") return out;

  for (const [k, v] of Object.entries(obj)) {
    const n = Number(k);
    if (Number.isFinite(n)) out[n] = v as T;
  }
  return out;
}

export default function SensorManagement() {
  const navigate = useNavigate();
  const [searchParams] = useSearchParams();
  const requestedSerial = searchParams.get("serial");

  const [sensor, setSensor] = useState<SensorValue>("accelerometer");
  const [timeframeMin, setTimeframeMin] = useState<number>(60);
  const [channel, setChannel] = useState<string>("all");

  const [nodes, setNodes] = useState<NodeRecord[]>([]);
  const [nodesStatus, setNodesStatus] = useState<string>("");
  const [selectedNodeLabel, setSelectedNodeLabel] = useState<string>("");

  useEffect(() => setChannel("all"), [sensor]);

  useEffect(() => {
    let mounted = true;

    async function loadNodeList() {
      try {
        const res = await getNodes();
        if (!mounted) return;

        const nextNodes = res.nodes ?? [];
        setNodes(nextNodes);
        setNodesStatus("");
      } catch (e: any) {
        if (!mounted) return;

        setNodes([]);
        setNodesStatus(`Node list load failed: ${e?.message ?? "Unknown error"}`);
      }
    }

    loadNodeList();
  }, []);

  useEffect(() => {
    if (!nodes.length) {
      setSelectedNodeLabel("");
      return;
    }

    if (requestedSerial) {
      const matched = nodes.find((n) => n.serial === requestedSerial);
      if (matched) {
        setSelectedNodeLabel(matched.label);
        return;
      }
    }

    setSelectedNodeLabel((prev) => {
      if (nodes.some((n) => n.label === prev)) return prev;
      return nodes[0].label;
    });
  }, [nodes, requestedSerial]);

  const selectedNode = useMemo(
    () => nodes.find((n) => n.label === selectedNodeLabel) ?? null,
    [nodes, selectedNodeLabel]
  );

  const nodeId = selectedNode?.node_id ?? 0;
  const nodeKey = selectedNode?.serial ?? "none";
  const isOnline = selectedNode?.online ?? false;

  const [metaByNode, setMetaByNode] = useState<Record<number, Record<SensorValue, SensorMeta>>>(() => {
    const cached = loadCachedSettings();
    return cached?.meta ?? {};
  });

  const [configByNode, setConfigByNode] = useState<Record<number, Record<SensorValue, SensorConfig>>>(() => {
    const cached = loadCachedSettings();
    return cached?.config ?? {};
  });

  const [settingsStatus, setSettingsStatus] = useState<string>("");

  useEffect(() => {
    async function loadSettingsFromBackend() {
      try {
        const json = await getSettings();

        const rawMetaByNode = normalizeNodeKeyedObject<Record<SensorValue, SensorMeta>>(json.meta);
        const rawConfigByNode = normalizeNodeKeyedObject<Record<SensorValue, SensorConfig>>(json.config);

        const nextMetaByNode: Record<number, Record<SensorValue, SensorMeta>> = {};
        const nextConfigByNode: Record<number, Record<SensorValue, SensorConfig>> = {};

        const nodeIds = new Set<number>([
          ...Object.keys(rawMetaByNode).map(Number),
          ...Object.keys(rawConfigByNode).map(Number),
        ]);

        nodeIds.forEach((n) => {
          nextMetaByNode[n] = { ...FALLBACK_META, ...(rawMetaByNode[n] ?? {}) };
          nextConfigByNode[n] = { ...FALLBACK_CONFIG, ...(rawConfigByNode[n] ?? {}) };
        });

        setMetaByNode(nextMetaByNode);
        setConfigByNode(nextConfigByNode);
        saveCachedSettings(nextMetaByNode, nextConfigByNode);

        setSettingsStatus("");
      } catch (e: any) {
        console.error(e);

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

    loadSettingsFromBackend();
  }, []);

  async function saveAllSettings(
    nextMetaByNode: Record<number, Record<SensorValue, SensorMeta>>,
    nextConfigByNode: Record<number, Record<SensorValue, SensorConfig>>
  ) {
    try {
      setSettingsStatus("Saving…");
      await putSettings({ meta: nextMetaByNode, config: nextConfigByNode });
      saveCachedSettings(nextMetaByNode, nextConfigByNode);
      setSettingsStatus("");
    } catch (e: any) {
      console.error(e);
      setSettingsStatus(`Save failed: ${e?.message ?? "Unknown error"}`);
    }
  }

  function ensureNodeDefaults(n: number) {
    if (!n) return;

    setMetaByNode((prev) =>
      prev[n] ? prev : { ...prev, [n]: { ...FALLBACK_META } }
    );

    setConfigByNode((prev) =>
      prev[n] ? prev : { ...prev, [n]: { ...FALLBACK_CONFIG } }
    );
  }

  useEffect(() => {
    if (nodeId) ensureNodeDefaults(nodeId);
  }, [nodeId]);

  function handleSaveMeta(updated: SensorMeta) {
    if (!nodeId) return;

    const currentNodeMeta = metaByNode[nodeId] ?? FALLBACK_META;
    const nextNodeMeta = { ...currentNodeMeta, [sensor]: updated };

    const nextMetaByNode = { ...metaByNode, [nodeId]: nextNodeMeta };
    setMetaByNode(nextMetaByNode);

    saveAllSettings(nextMetaByNode, configByNode);
  }

  function handleSaveConfig(updated: SensorConfig) {
    if (!nodeId) return;

    const currentNodeConfig = configByNode[nodeId] ?? FALLBACK_CONFIG;
    const nextNodeConfig = { ...currentNodeConfig, [sensor]: updated };

    const nextConfigByNode = { ...configByNode, [nodeId]: nextNodeConfig };
    setConfigByNode(nextConfigByNode);

    saveAllSettings(metaByNode, nextConfigByNode);
  }

  const metaForNode = nodeId ? (metaByNode[nodeId] ?? FALLBACK_META) : FALLBACK_META;
  const configForNode = nodeId ? (configByNode[nodeId] ?? FALLBACK_CONFIG) : FALLBACK_CONFIG;

  const meta = metaForNode[sensor];
  const config = configForNode[sensor];

  const [apiData, setApiData] = useState<ApiResponse | null>(null);
  const [plotStatus, setPlotStatus] = useState("Loading…");

  useEffect(() => {
    async function loadPlot() {
      if (!nodeId) {
        setApiData(null);
        setPlotStatus("No node selected");
        return;
      }

      const cacheKey = plotCacheKey({
        nodeKey,
        sensor,
        minutes: timeframeMin,
        channel,
      });

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
          node: nodeId,
          minutes: timeframeMin,
          channel,
        });

        setApiData(json);
        setPlotStatus("Loaded");
        savePlotCacheEntry(cacheKey, json);
      } catch (err: any) {
        console.error(err);

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
  }, [nodeId, nodeKey, sensor, channel, timeframeMin]);

  return (
    <div className="sc-page">
      <div className="top-selectors">
        <div className="control-box">
          <label className="control-label">Node</label>
          <select
            className="control-select"
            value={selectedNodeLabel}
            onChange={(e) => setSelectedNodeLabel(e.target.value)}
          >
            {nodes.length === 0 ? (
              <option value="">No nodes discovered</option>
            ) : (
              nodes.map((n) => (
                <option key={n.serial} value={n.label}>
                  {n.label}
                </option>
              ))
            )}
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

      {nodesStatus && <p style={{ marginTop: 8, marginBottom: 0 }}>{nodesStatus}</p>}

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
            <FaultLog node={nodeId || undefined} limit={10} />
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
        {apiData && selectedNode && (
          <SensorLineChart
            title={`${SENSOR_OPTIONS.find((s) => s.value === sensor)?.label ?? "Sensor"} (${selectedNode.label})`}
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