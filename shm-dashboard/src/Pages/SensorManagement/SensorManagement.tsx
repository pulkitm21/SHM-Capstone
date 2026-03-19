import { useEffect, useMemo, useState } from "react";
import { useSearchParams } from "react-router-dom";

import SensorInfoCard, { type SensorMeta } from "../../components/SensorInfo/SensorInfo";
import SensorConfigCard, {
  type HpfValue,
  type SensorConfig,
} from "../../components/SensorConfig/SensorConfig";
import FaultLog from "../../components/FaultLog/Log";
import SensorLineChart from "../../components/SensorPlot/SensorPlot";
import NodeTable from "../../components/NodeTable/NodeTable";
import SensorCardGrid from "../../components/SensorCardGrid/SensorCardGrid";

import {
  getSettings,
  putSettings,
  putAccelerometerHpf,
  getSensorData,
  getNodes,
  getFaults,
  type ApiResponse,
  type FaultRow,
  type NodeRecord,
} from "../../services/api";

import {
  UI_PREVIEW_MODE,
  PREVIEW_CONFIG_BY_NODE,
  PREVIEW_FAULTS_BY_SERIAL,
  PREVIEW_META_BY_NODE,
  PREVIEW_NODES,
  buildPreviewPlotData,
  type SensorValue,
} from "./SensorManagement.preview";

import "./SensorManagement.css";

const SETTINGS_CACHE_KEY = "shm_settings_cache";
const PLOT_CACHE_KEY = "shm_plot_cache";

type PlotCacheRecord = {
  savedAt: string;
  data: ApiResponse;
};

type NodeFaultSummary = {
  total: number;
  critical: number;
  warning: number;
  latest: FaultRow | null;
};

const SENSOR_DEFINITIONS: {
  label: string;
  value: SensorValue;
}[] = [
  {
    label: "Accelerometer",
    value: "accelerometer",
  },
  {
    label: "Inclinometer",
    value: "inclinometer",
  },
  {
    label: "Temperature",
    value: "temperature",
  },
];

const TIMEFRAME_OPTIONS = [
  { label: "1 hour", minutes: 60 },
  { label: "6 hours", minutes: 360 },
  { label: "12 hours", minutes: 720 },
  { label: "1 day", minutes: 1440 },
] as const;

const CHANNELS_BY_SENSOR: Record<SensorValue, { label: string; value: string }[]> = {
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
};

const EMPTY_SENSOR_CONFIG: SensorConfig = {
  highPassFilterDesired: "none",
  highPassFilterApplied: null,
  highPassFilterStatus: "unknown",
  lastRequestId: undefined,
  lastAckAt: null,
};

const FALLBACK_CONFIG: Record<SensorValue, SensorConfig> = {
  accelerometer: { ...EMPTY_SENSOR_CONFIG },
  inclinometer: { ...EMPTY_SENSOR_CONFIG },
  temperature: { ...EMPTY_SENSOR_CONFIG },
};

function loadCachedSettings():
  | {
      savedAt: string;
      meta: Record<number, Record<SensorValue, SensorMeta>>;
      config: Record<number, Record<SensorValue, SensorConfig>>;
    }
  | null {
  try {
    const raw = localStorage.getItem(SETTINGS_CACHE_KEY);
    if (!raw) return null;
    return JSON.parse(raw);
  } catch {
    return null;
  }
}

function saveCachedSettings(
  metaByNode: Record<number, Record<SensorValue, SensorMeta>>,
  configByNode: Record<number, Record<SensorValue, SensorConfig>>
) {
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
    // Ignore cache write failures.
  }
}

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
  } catch {
    // Ignore cache write failures.
  }
}

function normalizeNodeKeyedObject<T>(obj: unknown): Record<number, T> {
  const out: Record<number, T> = {};
  if (!obj || typeof obj !== "object") return out;

  for (const [k, v] of Object.entries(obj)) {
    const n = Number(k);
    if (Number.isFinite(n)) out[n] = v as T;
  }

  return out;
}

function normalizeSensorConfig(raw: any): SensorConfig {
  const desired = raw?.highPassFilterDesired ?? raw?.highPassFilter ?? "none";
  const applied =
    raw?.highPassFilterApplied ??
    raw?.applied?.highPassFilter ??
    raw?.highPassFilterAppliedValue ??
    null;

  const status =
    raw?.highPassFilterStatus ??
    raw?.sync_status ??
    (applied === null ? "unknown" : desired === applied ? "synced" : "pending");

  return {
    highPassFilterDesired: desired === "on" ? "on" : "none",
    highPassFilterApplied: applied === "on" || applied === "none" ? applied : null,
    highPassFilterStatus:
      status === "synced" || status === "pending" || status === "failed" ? status : "unknown",
    lastRequestId:
      typeof raw?.lastRequestId === "string"
        ? raw.lastRequestId
        : typeof raw?.request_id === "string"
          ? raw.request_id
          : undefined,
    lastAckAt:
      typeof raw?.lastAckAt === "string"
        ? raw.lastAckAt
        : typeof raw?.acked_at === "string"
          ? raw.acked_at
          : null,
  };
}

function normalizeSensorConfigMap(raw: any): Record<SensorValue, SensorConfig> {
  return {
    accelerometer: normalizeSensorConfig(raw?.accelerometer),
    inclinometer: normalizeSensorConfig(raw?.inclinometer),
    temperature: normalizeSensorConfig(raw?.temperature),
  };
}

function getSimpleSensorStatus(
  faults: FaultRow[],
  sensor: SensorValue,
  online: boolean
): { status: "online" | "offline" | "warning"; count: number } {
  const relevant = faults.filter((f) => String(f.sensor_type || "").toLowerCase() === sensor);

  if (!online) {
    return { status: "offline", count: relevant.length };
  }

  if (relevant.length > 0) {
    return { status: "warning", count: relevant.length };
  }

  return { status: "online", count: 0 };
}

function getNodeFaultSummary(faults: FaultRow[]): NodeFaultSummary {
  return {
    total: faults.length,
    critical: faults.filter((f) => Number(f.severity) >= 3).length,
    warning: faults.filter((f) => Number(f.severity) === 2).length,
    latest: faults[0] ?? null,
  };
}

export default function SensorManagement() {
  const [searchParams] = useSearchParams();
  const requestedSerial = searchParams.get("serial");

  const [sensor, setSensor] = useState<SensorValue>("accelerometer");
  const [timeframeMin, setTimeframeMin] = useState<number>(60);
  const [channel, setChannel] = useState<string>("all");

  const [nodes, setNodes] = useState<NodeRecord[]>([]);
  const [nodesStatus, setNodesStatus] = useState<string>("");
  const [selectedNodeLabel, setSelectedNodeLabel] = useState<string>("");

  const [metaByNode, setMetaByNode] = useState<Record<number, Record<SensorValue, SensorMeta>>>(() => {
    const cached = loadCachedSettings();
    return cached?.meta ?? {};
  });

  const [configByNode, setConfigByNode] = useState<
    Record<number, Record<SensorValue, SensorConfig>>
  >(() => {
    const cached = loadCachedSettings();
    return cached?.config ?? {};
  });

  const [settingsStatus, setSettingsStatus] = useState<string>("");
  const [faultStatus, setFaultStatus] = useState<string>("");
  const [nodeFaults, setNodeFaults] = useState<FaultRow[]>([]);
  const [allFaultsBySerial, setAllFaultsBySerial] = useState<Record<string, FaultRow[]>>({});

  const [apiData, setApiData] = useState<ApiResponse | null>(null);
  const [plotStatus, setPlotStatus] = useState("Loading…");

  useEffect(() => {
    setChannel("all");
  }, [sensor]);

  useEffect(() => {
    if (UI_PREVIEW_MODE) {
      setNodes(PREVIEW_NODES);
      setNodesStatus("UI preview mode: showing mock nodes while backend loading is disabled.");
      return;
    }

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

    return () => {
      mounted = false;
    };
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

  useEffect(() => {
    if (UI_PREVIEW_MODE) {
      setMetaByNode(PREVIEW_META_BY_NODE);
      setConfigByNode(PREVIEW_CONFIG_BY_NODE);
      setSettingsStatus("UI preview mode: showing mock sensor metadata and configuration.");
      return;
    }

    async function loadSettingsFromBackend() {
      try {
        const json = await getSettings();

        const rawMetaByNode = normalizeNodeKeyedObject<Record<SensorValue, SensorMeta>>(json.meta);
        const rawConfigByNode = normalizeNodeKeyedObject<any>(json.config);

        const nextMetaByNode: Record<number, Record<SensorValue, SensorMeta>> = {};
        const nextConfigByNode: Record<number, Record<SensorValue, SensorConfig>> = {};

        const nodeIds = new Set<number>([
          ...Object.keys(rawMetaByNode).map(Number),
          ...Object.keys(rawConfigByNode).map(Number),
        ]);

        nodeIds.forEach((n) => {
          nextMetaByNode[n] = { ...FALLBACK_META, ...(rawMetaByNode[n] ?? {}) };
          nextConfigByNode[n] = normalizeSensorConfigMap(rawConfigByNode[n]);
        });

        setMetaByNode(nextMetaByNode);
        setConfigByNode(nextConfigByNode);
        saveCachedSettings(nextMetaByNode, nextConfigByNode);
        setSettingsStatus("");
      } catch (e: any) {
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

  function ensureNodeDefaults(n: number) {
    if (!n) return;

    setMetaByNode((prev) => (prev[n] ? prev : { ...prev, [n]: { ...FALLBACK_META } }));
    setConfigByNode((prev) => (prev[n] ? prev : { ...prev, [n]: { ...FALLBACK_CONFIG } }));
  }

  useEffect(() => {
    if (nodeId) ensureNodeDefaults(nodeId);
  }, [nodeId]);

  async function saveAllSettings(
    nextMetaByNode: Record<number, Record<SensorValue, SensorMeta>>,
    nextConfigByNode: Record<number, Record<SensorValue, SensorConfig>>
  ) {
    if (UI_PREVIEW_MODE) {
      setMetaByNode(nextMetaByNode);
      setConfigByNode(nextConfigByNode);
      setSettingsStatus("UI preview mode: config changes are local only and not sent to the backend.");
      return;
    }

    try {
      setSettingsStatus("Saving…");
      await putSettings({ meta: nextMetaByNode, config: nextConfigByNode });
      saveCachedSettings(nextMetaByNode, nextConfigByNode);
      setSettingsStatus("");
    } catch (e: any) {
      setSettingsStatus(`Save failed: ${e?.message ?? "Unknown error"}`);
    }
  }

  function handleSaveMeta(updated: SensorMeta) {
    if (!nodeId) return;

    const currentNodeMeta = metaByNode[nodeId] ?? FALLBACK_META;
    const nextNodeMeta = { ...currentNodeMeta, [sensor]: updated };
    const nextMetaByNode = { ...metaByNode, [nodeId]: nextNodeMeta };

    setMetaByNode(nextMetaByNode);
    void saveAllSettings(nextMetaByNode, configByNode);
  }

  async function handleSaveConfig(updatedDesired: HpfValue) {
    if (!nodeId || !selectedNode) return;

    const currentNodeConfig = configByNode[nodeId] ?? FALLBACK_CONFIG;
    const currentSensorConfig = currentNodeConfig[sensor] ?? EMPTY_SENSOR_CONFIG;

    const nextSensorConfig: SensorConfig = {
      ...currentSensorConfig,
      highPassFilterDesired: updatedDesired,
      highPassFilterApplied: UI_PREVIEW_MODE ? updatedDesired : currentSensorConfig.highPassFilterApplied,
      highPassFilterStatus: UI_PREVIEW_MODE ? "synced" : "pending",
      lastAckAt: UI_PREVIEW_MODE ? new Date().toISOString() : currentSensorConfig.lastAckAt,
    };

    const nextNodeConfig: Record<SensorValue, SensorConfig> = {
      ...currentNodeConfig,
      [sensor]: nextSensorConfig,
    };

    const nextConfigByNode: Record<number, Record<SensorValue, SensorConfig>> = {
      ...configByNode,
      [nodeId]: nextNodeConfig,
    };

    if (UI_PREVIEW_MODE) {
      setConfigByNode(nextConfigByNode);
      setSettingsStatus(
        `UI preview mode: ${sensor} configuration updated locally only for frontend testing.`
      );
      return;
    }

    if (sensor !== "accelerometer") {
      setConfigByNode(nextConfigByNode);
      await saveAllSettings(metaByNode, nextConfigByNode);
      return;
    }

    try {
      setConfigByNode(nextConfigByNode);
      setSettingsStatus(selectedNode.online ? "Applying HPF…" : "Saving desired config…");

      const res = await putAccelerometerHpf(nodeId, {
        highPassFilterDesired: updatedDesired,
      });

      const appliedAccelConfig: SensorConfig = {
        highPassFilterDesired: res.desired.highPassFilter,
        highPassFilterApplied: res.applied.highPassFilter,
        highPassFilterStatus: res.sync_status,
        lastRequestId: res.request_id,
        lastAckAt: res.acked_at ?? null,
      };

      setConfigByNode((prev) => ({
        ...prev,
        [nodeId]: {
          ...(prev[nodeId] ?? FALLBACK_CONFIG),
          accelerometer: appliedAccelConfig,
        },
      }));

      setSettingsStatus("");
    } catch (e: any) {
      setConfigByNode((prev) => ({
        ...prev,
        [nodeId]: {
          ...(prev[nodeId] ?? FALLBACK_CONFIG),
          accelerometer: {
            ...nextSensorConfig,
            highPassFilterStatus: "failed",
          },
        },
      }));

      setSettingsStatus(`HPF apply failed: ${e?.message ?? "Unknown error"}`);
    }
  }

  useEffect(() => {
    if (UI_PREVIEW_MODE) {
      if (!selectedNode?.serial) {
        setNodeFaults([]);
        setFaultStatus("");
        return;
      }

      setNodeFaults(PREVIEW_FAULTS_BY_SERIAL[selectedNode.serial] ?? []);
      setFaultStatus("UI preview mode: showing mock faults while backend fault loading is disabled.");
      return;
    }

    const controller = new AbortController();

    async function loadNodeFaults() {
      if (!selectedNode?.serial) {
        setNodeFaults([]);
        setFaultStatus("");
        return;
      }

      try {
        setFaultStatus("Loading faults…");

        const res = await getFaults(
          {
            serial_number: selectedNode.serial,
            limit: 50,
          },
          controller.signal
        );

        setNodeFaults(res.faults ?? []);
        setFaultStatus("");
      } catch (e: any) {
        setNodeFaults([]);
        setFaultStatus(`Fault summary unavailable: ${e?.message ?? "Unknown error"}`);
      }
    }

    loadNodeFaults();

    return () => controller.abort();
  }, [selectedNode?.serial]);

  useEffect(() => {
    if (!nodes.length) {
      setAllFaultsBySerial({});
      return;
    }

    if (UI_PREVIEW_MODE) {
      const previewMap: Record<string, FaultRow[]> = {};
      nodes.forEach((node) => {
        previewMap[node.serial] = PREVIEW_FAULTS_BY_SERIAL[node.serial] ?? [];
      });
      setAllFaultsBySerial(previewMap);
      return;
    }

    let cancelled = false;

    async function loadFaultCountsForTable() {
      try {
        const entries = await Promise.all(
          nodes.map(async (node) => {
            try {
              const res = await getFaults({
                serial_number: node.serial,
                limit: 50,
              });
              return [node.serial, res.faults ?? []] as const;
            } catch {
              return [node.serial, []] as const;
            }
          })
        );

        if (cancelled) return;
        setAllFaultsBySerial(Object.fromEntries(entries));
      } catch {
        if (cancelled) return;
        setAllFaultsBySerial({});
      }
    }

    void loadFaultCountsForTable();

    return () => {
      cancelled = true;
    };
  }, [nodes]);

  useEffect(() => {
    if (UI_PREVIEW_MODE) {
      if (!nodeId) {
        setApiData(null);
        setPlotStatus("No node selected");
        return;
      }

      setApiData(buildPreviewPlotData(sensor));
      setPlotStatus("UI preview mode: showing mock trend data while backend plot loading is disabled.");
      return;
    }

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

    void loadPlot();
  }, [nodeId, nodeKey, sensor, channel, timeframeMin]);

const metaForNode = nodeId ? (metaByNode[nodeId] ?? FALLBACK_META) : FALLBACK_META;
const configForNode = nodeId ? (configByNode[nodeId] ?? FALLBACK_CONFIG) : FALLBACK_CONFIG;

const meta = metaForNode[sensor];
const config = configForNode[sensor];

  const nodeFaultSummary = useMemo(() => getNodeFaultSummary(nodeFaults), [nodeFaults]);

  const sensorCards = useMemo(() => {
    return SENSOR_DEFINITIONS.map((sensorDef) => {
      const sensorMeta = metaForNode[sensorDef.value] ?? FALLBACK_META[sensorDef.value];
      const summary = getSimpleSensorStatus(nodeFaults, sensorDef.value, isOnline);

      return {
        label: sensorDef.label,
        value: sensorDef.value,
        model: sensorMeta.model,
        status: summary.status,
      };
    });
  }, [metaForNode, nodeFaults, isOnline]);

  const selectedSensorDef =
    SENSOR_DEFINITIONS.find((entry) => entry.value === sensor) ?? SENSOR_DEFINITIONS[0];

  return (
    <div className="sm-page">
      <div className="sm-toolbar">
        <div>
          <h1 className="sm-page-title">Sensor Management</h1>
          {nodesStatus && <p className="sm-inline-status">{nodesStatus}</p>}
          {settingsStatus && <p className="sm-inline-status">{settingsStatus}</p>}
          {faultStatus && <p className="sm-inline-status">{faultStatus}</p>}
        </div>
      </div>

      {!selectedNode ? (
        <div className="sm-empty-state">
          <h2>No node selected</h2>
          <p>Select a node from the table to inspect sensors, plots, and configuration.</p>
        </div>
      ) : (
        <div className="sm-layout">
          <NodeTable
            nodes={nodes}
            selectedNodeLabel={selectedNodeLabel}
            onSelectNode={setSelectedNodeLabel}
            faultsBySerial={allFaultsBySerial}
          />

          <section className="sm-main">
            <SensorCardGrid
              sensors={sensorCards}
              selectedSensor={sensor}
              onSelectSensor={setSensor}
            />

            <div className="sm-detail-grid">
              <div className="sm-detail-column">
                <SensorInfoCard meta={meta} onSave={handleSaveMeta} />
              </div>

              <div className="sm-detail-column">
                <SensorConfigCard
                  title={`${selectedSensorDef.label} Configuration`}
                  config={config}
                  onSave={handleSaveConfig}
                  disabled={!selectedNode}
                />
              </div>
            </div>

            <div className="sm-panel sm-full-width-panel">
              <div className="sm-plot-toolbar">
                <div>
                  <h2 className="sm-panel-title">{selectedSensorDef.label} Trend</h2>
                  <p className="sm-panel-subtitle">
                    Recent data for {selectedNode.serial}
                  </p>
                </div>

                <div className="sm-filter-row">
                  <div className="sm-filter-group">
                    <label htmlFor="sm-timeframe">Window</label>
                    <select
                      id="sm-timeframe"
                      value={timeframeMin}
                      onChange={(e) => setTimeframeMin(Number(e.target.value))}
                    >
                      {TIMEFRAME_OPTIONS.map((option) => (
                        <option key={option.minutes} value={option.minutes}>
                          {option.label}
                        </option>
                      ))}
                    </select>
                  </div>

                  <div className="sm-filter-group">
                    <label htmlFor="sm-channel">Channel</label>
                    <select
                      id="sm-channel"
                      value={channel}
                      onChange={(e) => setChannel(e.target.value)}
                    >
                      {CHANNELS_BY_SENSOR[sensor].map((option) => (
                        <option key={option.value} value={option.value}>
                          {option.label}
                        </option>
                      ))}
                    </select>
                  </div>
                </div>
              </div>

              <div className="sm-plot-status">{plotStatus}</div>

              <div className="sm-plot-wrap">
                {apiData && selectedNode ? (
                  <SensorLineChart
                    title={`${selectedSensorDef.label} (${selectedNode.serial})`}
                    sensorKey={apiData.sensor ?? sensor}
                    unit={apiData.unit ?? ""}
                    points={apiData.points.map((p) => ({ ts: p.t, value: p.v }))}
                    height={420}
                  />
                ) : (
                  <div className="sm-plot-empty">No plot data available for the current selection.</div>
                )}
              </div>
            </div>
<FaultLog
  variant="node"
  serial_number={selectedNode.serial}
  limit={5}
  previewMode={UI_PREVIEW_MODE}
  previewFaults={PREVIEW_FAULTS_BY_SERIAL[selectedNode.serial] ?? []}
/>
          </section>
        </div>
      )}
    </div>
  );
}