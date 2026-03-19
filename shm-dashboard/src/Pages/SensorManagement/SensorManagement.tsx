import { useEffect, useMemo, useState } from "react";
import { useSearchParams } from "react-router-dom";

import SensorConfigCard, {
  type SensorConfig,
} from "../../components/SensorConfig/SensorConfig";
import FaultLog from "../../components/FaultLog/Log";
import SensorLineChart from "../../components/SensorPlot/SensorPlot";
import NodeTable from "../../components/NodeTable/NodeTable";
import SensorCardGrid from "../../components/SensorCardGrid/SensorCardGrid";

import {
  getSettings,
  applyAccelerometerConfig,
  sendNodeControl,
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

import type { SensorMeta } from "../../components/SensorInfo/SensorInfo";
import "./SensorManagement.css";

const SETTINGS_CACHE_KEY = "shm_settings_cache";
const PLOT_CACHE_KEY = "shm_plot_cache";

type PlotCacheRecord = {
  savedAt: string;
  data: ApiResponse;
};

const SENSOR_DEFINITIONS: {
  label: string;
  value: SensorValue;
}[] = [
  { label: "Accelerometer", value: "accelerometer" },
  { label: "Inclinometer", value: "inclinometer" },
  { label: "Temperature", value: "temperature" },
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
  odr_index: 2,
  range: 1,
  hpf_corner: 0,
  desired_odr_index: 2,
  desired_range: 1,
  desired_hpf_corner: 0,
  applied_odr_index: null,
  applied_range: null,
  applied_hpf_corner: null,
  current_state: "unknown",
  pending_seq: null,
  applied_seq: null,
  last_ack_at: null,
  sync_status: "unknown",
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
  return {
    odr_index: raw?.odr_index ?? 2,
    range: raw?.range ?? 1,
    hpf_corner: raw?.hpf_corner ?? 0,
    desired_odr_index: raw?.desired_odr_index ?? raw?.odr_index ?? 2,
    desired_range: raw?.desired_range ?? raw?.range ?? 1,
    desired_hpf_corner: raw?.desired_hpf_corner ?? raw?.hpf_corner ?? 0,
    applied_odr_index: raw?.applied_odr_index ?? null,
    applied_range: raw?.applied_range ?? null,
    applied_hpf_corner: raw?.applied_hpf_corner ?? null,
    current_state: raw?.current_state ?? "unknown",
    pending_seq: raw?.pending_seq ?? null,
    applied_seq: raw?.applied_seq ?? null,
    last_ack_at: raw?.last_ack_at ?? raw?.acked_at ?? null,
    sync_status: raw?.sync_status ?? "unknown",
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

  const [configByNode, setConfigByNode] = useState<Record<number, Record<SensorValue, SensorConfig>>>(() => {
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

        setNodes(res.nodes ?? []);
        setNodesStatus("");
      } catch (e: any) {
        if (!mounted) return;

        setNodes([]);
        setNodesStatus(`Node list load failed: ${e?.message ?? "Unknown error"}`);
      }
    }

    void loadNodeList();

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

    void loadSettingsFromBackend();
  }, []);

  function ensureNodeDefaults(n: number) {
    if (!n) return;

    setMetaByNode((prev) => (prev[n] ? prev : { ...prev, [n]: { ...FALLBACK_META } }));
    setConfigByNode((prev) => (prev[n] ? prev : { ...prev, [n]: { ...FALLBACK_CONFIG } }));
  }

  useEffect(() => {
    if (nodeId) ensureNodeDefaults(nodeId);
  }, [nodeId]);

  async function handleApplyAccelerometerConfig(updated: {
    odr_index: 0 | 1 | 2;
    range: 1 | 2 | 3;
    hpf_corner: number;
  }) {
    if (!nodeId || !selectedNode) return;

    const currentNodeConfig = configByNode[nodeId] ?? FALLBACK_CONFIG;
    const currentAccelConfig = currentNodeConfig.accelerometer ?? EMPTY_SENSOR_CONFIG;

    const optimisticAccelConfig: SensorConfig = {
      ...currentAccelConfig,
      odr_index: updated.odr_index,
      range: updated.range,
      hpf_corner: updated.hpf_corner,
      desired_odr_index: updated.odr_index,
      desired_range: updated.range,
      desired_hpf_corner: updated.hpf_corner,
      sync_status: UI_PREVIEW_MODE ? "synced" : "pending",
      last_ack_at: UI_PREVIEW_MODE ? new Date().toISOString() : currentAccelConfig.last_ack_at,
      current_state: currentAccelConfig.current_state ?? "unknown",
    };

    const nextConfigByNode: Record<number, Record<SensorValue, SensorConfig>> = {
      ...configByNode,
      [nodeId]: {
        ...currentNodeConfig,
        accelerometer: optimisticAccelConfig,
      },
    };

    if (UI_PREVIEW_MODE) {
      setConfigByNode(nextConfigByNode);
      setSettingsStatus("UI preview mode: accelerometer config updated locally only.");
      return;
    }

    try {
      setConfigByNode(nextConfigByNode);
      setSettingsStatus(selectedNode.online ? "Applying accelerometer config…" : "Saving desired config…");

      const res = await applyAccelerometerConfig(nodeId, updated);

      const appliedAccelConfig: SensorConfig = {
        ...currentAccelConfig,
        odr_index: res.desired.odr_index,
        range: res.desired.range,
        hpf_corner: res.desired.hpf_corner,
        desired_odr_index: res.desired.odr_index,
        desired_range: res.desired.range,
        desired_hpf_corner: res.desired.hpf_corner,
        applied_odr_index: res.applied.odr_index,
        applied_range: res.applied.range,
        applied_hpf_corner: res.applied.hpf_corner,
        current_state: res.current_state,
        pending_seq: res.pending_seq,
        applied_seq: res.applied_seq,
        last_ack_at: res.acked_at ?? null,
        sync_status: res.sync_status,
      };

      setConfigByNode((prev) => ({
        ...prev,
        [nodeId]: {
          ...(prev[nodeId] ?? FALLBACK_CONFIG),
          accelerometer: appliedAccelConfig,
        },
      }));

      setSettingsStatus("Accelerometer config request accepted.");
    } catch (e: any) {
      setConfigByNode((prev) => ({
        ...prev,
        [nodeId]: {
          ...(prev[nodeId] ?? FALLBACK_CONFIG),
          accelerometer: {
            ...optimisticAccelConfig,
            sync_status: "failed",
          },
        },
      }));

      setSettingsStatus(`Accelerometer config apply failed: ${e?.message ?? "Unknown error"}`);
    }
  }

  async function handleNodeControl(cmd: "start" | "stop") {
    if (!nodeId || !selectedNode) return;

    if (UI_PREVIEW_MODE) {
      setConfigByNode((prev) => ({
        ...prev,
        [nodeId]: {
          ...(prev[nodeId] ?? FALLBACK_CONFIG),
          accelerometer: {
            ...(prev[nodeId]?.accelerometer ?? EMPTY_SENSOR_CONFIG),
            current_state: cmd === "start" ? "recording" : "configured",
          },
        },
      }));

      setSettingsStatus(`UI preview mode: node ${cmd} handled locally only.`);
      return;
    }

    try {
      setSettingsStatus(`${cmd === "start" ? "Starting" : "Stopping"} node…`);
      const res = await sendNodeControl(nodeId, { cmd });
      setSettingsStatus(`Node ${res.cmd} request accepted.`);
    } catch (e: any) {
      setSettingsStatus(`Node ${cmd} failed: ${e?.message ?? "Unknown error"}`);
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

    void loadNodeFaults();

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

  const config = configForNode[sensor];

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

            <SensorConfigCard
              title={`${selectedSensorDef.label} Configuration`}
              config={config}
              onApply={handleApplyAccelerometerConfig}
              onStart={() => handleNodeControl("start")}
              onStop={() => handleNodeControl("stop")}
              disabled={!selectedNode || sensor !== "accelerometer"}
            />

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
                {apiData && selectedNode && Array.isArray(apiData.points) && apiData.points.length > 0 ? (
                  <SensorLineChart
                    title={`${selectedSensorDef.label} (${selectedNode.serial})`}
                    sensorKey={apiData.sensor ?? sensor}
                    unit={apiData.unit ?? ""}
                    points={apiData.points.map((p) => ({
                      ts: p.t,
                      value: p.v,
                    }))}
                    height={420}
                  />
                ) : (
                  <div className="sm-plot-empty">No plot data available for the selected view.</div>
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