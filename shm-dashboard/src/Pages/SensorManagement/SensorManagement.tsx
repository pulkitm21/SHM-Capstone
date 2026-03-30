import { useCallback, useEffect, useMemo, useState } from "react";
import { useSearchParams } from "react-router-dom";

import useAuth from "../../Auth/useAuth";
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
  getFaultSummary,
  getNodeSensorStatus,
  type ApiResponse,
  type FaultRow,
  type NodeRecord,
  type FaultSummaryResponse,
  type NodeSensorStatusResponse,
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
const SENSOR_STATUS_POLL_MS = 5000;

type PlotCacheRecord = {
  savedAt: string;
  data: ApiResponse;
};

type SensorStatusValue = "online" | "offline" | "warning" | "idle";

type SensorStatusMap = Record<
  SensorValue,
  {
    status: SensorStatusValue;
    count: number;
    hasData: boolean;
    lastDataTs: string | null;
    lastFaultDescription: string | null;
  }
>;

const SENSOR_DEFINITIONS: {
  label: string;
  value: SensorValue;
}[] = [
  { label: "Accelerometer", value: "accelerometer" },
  { label: "Inclinometer", value: "inclinometer" },
  { label: "Temperature", value: "temperature" },
];

const TIMEFRAME_OPTIONS_BY_SENSOR: Record<
  SensorValue,
  ReadonlyArray<{ label: string; minutes: number }>
> = {
  accelerometer: [
    { label: "Current hour", minutes: 60 },
    { label: "Last 5 min", minutes: 5 },
    { label: "Last 1 min", minutes: 1 },
  ],
  inclinometer: [
    { label: "Current hour", minutes: 60 },
    { label: "Last 5 min", minutes: 5 },
    { label: "Last 1 min", minutes: 1 },
  ],
  temperature: [
    { label: "Current hour", minutes: 60 },
    { label: "Last 5 min", minutes: 5 },
    { label: "Last 1 min", minutes: 1 },
  ],
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
    orientation: "Pitch / Roll / Yaw",
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
  sync_status: null,
  pending_control_cmd: null,
  pending_control_seq: null,
  last_control_cmd: null,
  last_control_seq: null,
  last_control_ack_at: null,
  control_status: null,
};

const FALLBACK_CONFIG: Record<SensorValue, SensorConfig> = {
  accelerometer: { ...EMPTY_SENSOR_CONFIG },
  inclinometer: { ...EMPTY_SENSOR_CONFIG },
  temperature: { ...EMPTY_SENSOR_CONFIG },
};

// Load cached settings so the page can still render metadata/config offline.
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

// Save settings cache after successful backend sync.
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

// Build a stable cache key for one node/sensor/window plot request.
function plotCacheKey(params: {
  nodeKey: string;
  sensor: SensorValue;
  minutes: number;
}) {
  return `${params.nodeKey}|${params.sensor}|${params.minutes}`;
}

// Load cached plots from local storage.
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

// Check whether an API plot response contains usable data points.
function hasPlotPoints(data: ApiResponse | null): boolean {
  return !!data && Array.isArray(data.points) && data.points.length > 0;
}

// Save one plot response into local cache.
function savePlotCacheEntry(key: string, data: ApiResponse) {
  try {
    const all = loadPlotCache();
    all[key] = { savedAt: new Date().toISOString(), data };
    localStorage.setItem(PLOT_CACHE_KEY, JSON.stringify(all));
  } catch {
    // Ignore cache write failures.
  }
}

// Convert numeric-keyed JSON objects into number-indexed records.
function normalizeNodeKeyedObject<T>(obj: unknown): Record<number, T> {
  const out: Record<number, T> = {};
  if (!obj || typeof obj !== "object") return out;

  for (const [k, v] of Object.entries(obj)) {
    const n = Number(k);
    if (Number.isFinite(n)) out[n] = v as T;
  }

  return out;
}

// Normalize one backend sensor config object into the frontend shape.
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
}

// Normalize the per-node sensor config map.
function normalizeSensorConfigMap(raw: any): Record<SensorValue, SensorConfig> {
  return {
    accelerometer: normalizeSensorConfig(raw?.accelerometer),
    inclinometer: normalizeSensorConfig(raw?.inclinometer),
    temperature: normalizeSensorConfig(raw?.temperature),
  };
}

// Fallback sensor health used when the backend sensor-status endpoint is unavailable.
function buildFallbackSensorStatus(
  nodeOnline: boolean,
  faults: FaultRow[]
): SensorStatusMap {
  const buildOne = (sensor: SensorValue) => {
    const relevantActive = faults.filter(
      (f) =>
        String(f.sensor_type || "").toLowerCase() === sensor &&
        String(f.fault_status || "").toLowerCase() === "active"
    );

    if (!nodeOnline) {
      return {
        status: "offline" as const,
        count: relevantActive.length,
        hasData: false,
        lastDataTs: null,
        lastFaultDescription: relevantActive[0]?.description ?? null,
      };
    }

    if (relevantActive.length > 0) {
      return {
        status: "warning" as const,
        count: relevantActive.length,
        hasData: true,
        lastDataTs: null,
        lastFaultDescription: relevantActive[0]?.description ?? null,
      };
    }

    return {
      status: "idle" as const,
      count: 0,
      hasData: false,
      lastDataTs: null,
      lastFaultDescription: null,
    };
  };

  return {
    accelerometer: buildOne("accelerometer"),
    inclinometer: buildOne("inclinometer"),
    temperature: buildOne("temperature"),
  };
}

// Format a recent ISO timestamp into a short age label for sensor cards.
function formatSensorAgeLabel(ts: string | null): string {
  if (!ts) return "—";

  const parsed = new Date(ts);
  if (Number.isNaN(parsed.getTime())) return "—";

  const diffSeconds = Math.max(
    0,
    Math.floor((Date.now() - parsed.getTime()) / 1000)
  );

  if (diffSeconds < 60) return `${diffSeconds}s ago`;

  const diffMinutes = Math.floor(diffSeconds / 60);
  if (diffMinutes < 60) return `${diffMinutes}m ago`;

  const diffHours = Math.floor(diffMinutes / 60);
  if (diffHours < 24) return `${diffHours}h ago`;

  const diffDays = Math.floor(diffHours / 24);
  return `${diffDays}d ago`;
}

export default function SensorManagement() {
  const { isAdmin } = useAuth();
  const [searchParams] = useSearchParams();
  const requestedSerial = searchParams.get("serial");

  const [sensor, setSensor] = useState<SensorValue>("accelerometer");
  const [timeframeMin, setTimeframeMin] = useState<number>(1);

  const [nodes, setNodes] = useState<NodeRecord[]>([]);
  const [nodesStatus, setNodesStatus] = useState<string>("");
  const [selectedNodeLabel, setSelectedNodeLabel] = useState<string>("");

  const [metaByNode, setMetaByNode] = useState<
    Record<number, Record<SensorValue, SensorMeta>>
  >(() => {
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
  const [allFaultCountsBySerial, setAllFaultCountsBySerial] = useState<
    Record<string, number>
  >({});

  const [sensorStatusMap, setSensorStatusMap] = useState<SensorStatusMap>({
    accelerometer: {
      status: "offline",
      count: 0,
      hasData: false,
      lastDataTs: null,
      lastFaultDescription: null,
    },
    inclinometer: {
      status: "offline",
      count: 0,
      hasData: false,
      lastDataTs: null,
      lastFaultDescription: null,
    },
    temperature: {
      status: "offline",
      count: 0,
      hasData: false,
      lastDataTs: null,
      lastFaultDescription: null,
    },
  });

  const [apiData, setApiData] = useState<ApiResponse | null>(null);
  const [plotStatus, setPlotStatus] = useState("Loading…");
  const [plotRefreshKey, setPlotRefreshKey] = useState(0);
  const [plotLastUpdated, setPlotLastUpdated] = useState<Date | null>(null);

  // Refresh settings from backend and merge them with fallback defaults.
  const refreshSettingsFromBackend = useCallback(async () => {
    const json = await getSettings();

    const rawMetaByNode =
      normalizeNodeKeyedObject<Record<SensorValue, SensorMeta>>(json.meta);
    const rawConfigByNode = normalizeNodeKeyedObject<any>(json.config);

    const nextMetaByNode: Record<number, Record<SensorValue, SensorMeta>> = {};
    const nextConfigByNode: Record<number, Record<SensorValue, SensorConfig>> =
      {};

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
  }, []);

  // Load the live node list.
  useEffect(() => {
    if (UI_PREVIEW_MODE) {
      setNodes(PREVIEW_NODES);
      setNodesStatus(
        "UI preview mode: showing mock nodes while backend loading is disabled."
      );
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
        setNodesStatus(
          `Node list load failed: ${e?.message ?? "Unknown error"}`
        );
      }
    }

    void loadNodeList();

    return () => {
      mounted = false;
    };
  }, []);

  // Select the requested serial if present, otherwise keep current or default to the first node.
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

  // Load sensor metadata and saved configuration.
  useEffect(() => {
    if (UI_PREVIEW_MODE) {
      setMetaByNode(PREVIEW_META_BY_NODE);
      setConfigByNode(PREVIEW_CONFIG_BY_NODE);
      setSettingsStatus(
        "UI preview mode: showing mock sensor metadata and configuration."
      );
      return;
    }

    let mounted = true;

    async function loadSettings() {
      try {
        await refreshSettingsFromBackend();
        if (!mounted) return;
        setSettingsStatus("");
      } catch (e: any) {
        if (!mounted) return;

        const cached = loadCachedSettings();

        if (cached?.savedAt) {
          setSettingsStatus(
            `Backend unreachable — using cached settings (last synced: ${new Date(
              cached.savedAt
            ).toLocaleString()})`
          );
        } else {
          setSettingsStatus(
            `Settings load failed: ${e?.message ?? "Unknown error"}`
          );
        }
      }
    }

    void loadSettings();

    return () => {
      mounted = false;
    };
  }, [refreshSettingsFromBackend]);

  // Ensure the selected node always has fallback meta/config entries.
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

  // Apply accelerometer configuration optimistically, then publish to backend.
  async function handleApplyAccelerometerConfig(updated: {
    odr_index: 0 | 1 | 2;
    range: 1 | 2 | 3;
    hpf_corner: number;
  }) {
    if (!nodeId || !selectedNode || !isAdmin) return;

    const currentNodeConfig = configByNode[nodeId] ?? FALLBACK_CONFIG;

    const optimisticAccelConfig: SensorConfig = {
      ...(currentNodeConfig.accelerometer ?? EMPTY_SENSOR_CONFIG),
      odr_index: updated.odr_index,
      range: updated.range,
      hpf_corner: updated.hpf_corner,
      desired_odr_index: updated.odr_index,
      desired_range: updated.range,
      desired_hpf_corner: updated.hpf_corner,
    };

    const nextConfigByNode: Record<number, Record<SensorValue, SensorConfig>> =
      {
        ...configByNode,
        [nodeId]: {
          ...currentNodeConfig,
          accelerometer: optimisticAccelConfig,
        },
      };

    if (UI_PREVIEW_MODE) {
      setConfigByNode(nextConfigByNode);
      setSettingsStatus(
        "UI preview mode: accelerometer config updated locally only."
      );
      return;
    }

    try {
      setConfigByNode(nextConfigByNode);
      setSettingsStatus(
        selectedNode.online
          ? "Sending accelerometer config…"
          : "Saving desired config…"
      );
      await applyAccelerometerConfig(nodeId, updated);
      setSettingsStatus("Accelerometer config sent.");
    } catch (e: any) {
      setSettingsStatus(
        `Accelerometer config apply failed: ${e?.message ?? "Unknown error"}`
      );
    }
  }

  // Handle node runtime control commands (start/stop/reset)
  async function handleNodeControl(cmd: "start" | "stop" | "reset") {
    if (!nodeId || !selectedNode || !isAdmin) return;

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
      setSettingsStatus(
        cmd === "start"
          ? "Starting node…"
          : cmd === "stop"
            ? "Stopping node…"
            : "Resetting node…"
      );
      await sendNodeControl(nodeId, { cmd });

      setConfigByNode((prev) => ({
        ...prev,
        [nodeId]: {
          ...(prev[nodeId] ?? FALLBACK_CONFIG),
          accelerometer: {
            ...(prev[nodeId]?.accelerometer ?? EMPTY_SENSOR_CONFIG),
            current_state:
              cmd === "start"
                ? "recording"
                : cmd === "stop"
                  ? "configured"
                  : "unknown", // reset → unknown state until node reports back
          },
        },
      }));

      setSettingsStatus(`Node ${cmd} command sent.`);
    } catch (e: any) {
      setSettingsStatus(`Node ${cmd} failed: ${e?.message ?? "Unknown error"}`);
    }
  }

  // Load recent node-specific faults for the selected node.
  useEffect(() => {
    if (UI_PREVIEW_MODE) {
      if (!selectedNode?.serial) {
        setNodeFaults([]);
        setFaultStatus("");
        return;
      }

      setNodeFaults(PREVIEW_FAULTS_BY_SERIAL[selectedNode.serial] ?? []);
      setFaultStatus(
        "UI preview mode: showing mock faults while backend fault loading is disabled."
      );
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
        setFaultStatus(
          `Fault summary unavailable: ${e?.message ?? "Unknown error"}`
        );
      }
    }

    void loadNodeFaults();

    return () => controller.abort();
  }, [selectedNode?.serial]);

  // Load lightweight fault summaries for the node table.
  useEffect(() => {
    if (!nodes.length) {
      setAllFaultCountsBySerial({});
      return;
    }

    if (UI_PREVIEW_MODE) {
      const previewMap: Record<string, number> = {};
      nodes.forEach((node) => {
        previewMap[node.serial] = (PREVIEW_FAULTS_BY_SERIAL[node.serial] ?? []).filter(
          (fault) => String(fault.fault_status ?? "").toLowerCase() === "active"
        ).length;
      });
      setAllFaultCountsBySerial(previewMap);
      return;
    }

    let cancelled = false;

    async function loadFaultCountsForTable() {
      try {
        const res: FaultSummaryResponse = await getFaultSummary();

        if (cancelled) return;

        const counts: Record<string, number> = {};
        nodes.forEach((node) => {
          counts[node.serial] = res.by_serial[node.serial]?.active_count ?? 0;
        });

        setAllFaultCountsBySerial(counts);
      } catch {
        if (cancelled) return;
        setAllFaultCountsBySerial({});
      }
    }

    void loadFaultCountsForTable();

    return () => {
      cancelled = true;
    };
  }, [nodes]);

  useEffect(() => {
    const validOptions = TIMEFRAME_OPTIONS_BY_SENSOR[sensor];

    if (!validOptions.some((option) => option.minutes === timeframeMin)) {
      setTimeframeMin(validOptions[0].minutes);
    }
  }, [sensor, timeframeMin]);

  // Load backend-driven per-sensor status so each sensor can be independent of node status.
  useEffect(() => {
    if (!selectedNode) {
      setSensorStatusMap(buildFallbackSensorStatus(false, []));
      return;
    }

    if (UI_PREVIEW_MODE) {
      setSensorStatusMap(
        buildFallbackSensorStatus(selectedNode.online, nodeFaults)
      );
      return;
    }

    const controller = new AbortController();
    const currentNode = selectedNode;
    let cancelled = false;
    let loading = false;

    async function loadSensorStatuses() {
      if (loading) return;
      loading = true;

      try {
        const res: NodeSensorStatusResponse = await getNodeSensorStatus(
          currentNode.node_id,
          120,
          controller.signal
        );

        if (cancelled) return;

        setSensorStatusMap({
          accelerometer: {
            status: res.sensors.accelerometer.status,
            count: res.sensors.accelerometer.active_fault_count,
            hasData: res.sensors.accelerometer.has_data,
            lastDataTs: res.sensors.accelerometer.last_data_ts ?? null,
            lastFaultDescription:
              res.sensors.accelerometer.active_faults?.[0]?.description ?? null,
          },
          inclinometer: {
            status: res.sensors.inclinometer.status,
            count: res.sensors.inclinometer.active_fault_count,
            hasData: res.sensors.inclinometer.has_data,
            lastDataTs: res.sensors.inclinometer.last_data_ts ?? null,
            lastFaultDescription:
              res.sensors.inclinometer.active_faults?.[0]?.description ?? null,
          },
          temperature: {
            status: res.sensors.temperature.status,
            count: res.sensors.temperature.active_fault_count,
            hasData: res.sensors.temperature.has_data,
            lastDataTs: res.sensors.temperature.last_data_ts ?? null,
            lastFaultDescription:
              res.sensors.temperature.active_faults?.[0]?.description ?? null,
          },
        });
      } catch (e) {
        if (cancelled) return;
        console.error("Sensor status load failed:", e);
        setSensorStatusMap(
          buildFallbackSensorStatus(currentNode.online, nodeFaults)
        );
      } finally {
        loading = false;
      }
    }

    void loadSensorStatuses();

    const intervalId = window.setInterval(() => {
      void loadSensorStatuses();
    }, SENSOR_STATUS_POLL_MS);

    return () => {
      cancelled = true;
      controller.abort();
      window.clearInterval(intervalId);
    };
  }, [selectedNode, nodeFaults]);

  // Manually refresh the currently selected plot from the backend.
  function handleRefreshPlot() {
    setPlotRefreshKey((prev) => prev + 1);
  }

  // Load plot data for the currently selected sensor and time window.
  useEffect(() => {
    if (UI_PREVIEW_MODE) {
      if (!nodeId) {
        setApiData(null);
        setPlotStatus("No node selected");
        return;
      }

      setApiData(buildPreviewPlotData(sensor));
      setPlotLastUpdated(new Date());
      setPlotStatus(
        "UI preview mode: showing mock trend data while backend plot loading is disabled."
      );
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
      });

      const cache = loadPlotCache();
      const cachedEntry = cache[cacheKey];
      const isManualRefresh = plotRefreshKey > 0;

      if (!isManualRefresh && hasPlotPoints(cachedEntry?.data ?? null)) {
        setApiData(cachedEntry!.data);
        setPlotLastUpdated(new Date(cachedEntry!.savedAt));
        setPlotStatus(
          `Using cached plot (last synced: ${new Date(
            cachedEntry!.savedAt
          ).toLocaleString()})`
        );
      } else {
        setPlotStatus(isManualRefresh ? "Refreshing…" : "Loading…");
        if (!hasPlotPoints(cachedEntry?.data ?? null)) {
          setApiData(null);
        }
      }

      try {
        const endpoint = ENDPOINT_BY_SENSOR[sensor];
        const json = await getSensorData(endpoint, {
          node: nodeId,
          minutes: timeframeMin,
        });

        setApiData(json);
        setPlotLastUpdated(new Date());
        setPlotStatus("Loaded");
        savePlotCacheEntry(cacheKey, json);
      } catch (err: any) {
        if (hasPlotPoints(cachedEntry?.data ?? null)) {
          setPlotLastUpdated(new Date(cachedEntry!.savedAt));
          setPlotStatus(
            `Backend unreachable — showing cached plot (last synced: ${new Date(
              cachedEntry!.savedAt
            ).toLocaleString()})`
          );
          return;
        }

        setApiData(null);
        setPlotStatus(`Error: ${err?.message ?? "Unknown error"}`);
      }
    }

    void loadPlot();
  }, [nodeId, nodeKey, sensor, timeframeMin, plotRefreshKey]);

  const metaForNode = nodeId
    ? metaByNode[nodeId] ?? FALLBACK_META
    : FALLBACK_META;
  const configForNode = nodeId
    ? configByNode[nodeId] ?? FALLBACK_CONFIG
    : FALLBACK_CONFIG;
  const config = configForNode[sensor];

  // Build sensor cards from backend-provided per-sensor status.
  const sensorCards = useMemo(() => {
    return SENSOR_DEFINITIONS.map((sensorDef) => {
      const sensorMeta =
        metaForNode[sensorDef.value] ?? FALLBACK_META[sensorDef.value];
      const summary = sensorStatusMap[sensorDef.value];

      return {
        label: sensorDef.label,
        value: sensorDef.value,
        model: sensorMeta.model,
        status: summary.status,
        lastDataAgeText: formatSensorAgeLabel(summary.lastDataTs),
        activeFaultCount: summary.count,
        latestFaultText: summary.lastFaultDescription,
      };
    });
  }, [metaForNode, sensorStatusMap]);

  const selectedSensorDef =
    SENSOR_DEFINITIONS.find((entry) => entry.value === sensor) ??
    SENSOR_DEFINITIONS[0];

  const timeframeOptions = TIMEFRAME_OPTIONS_BY_SENSOR[sensor];
  const selectedSensorStatus = sensorStatusMap[sensor];

  const plotEmptyMessage = useMemo(() => {
    if (!selectedNode) {
      return "Select a node to view raw preview data.";
    }

    if (plotStatus.startsWith("Error:")) {
      return `Raw preview unavailable: ${plotStatus.replace(/^Error:\s*/, "")}`;
    }

    if (selectedSensorStatus.status === "offline") {
      return selectedSensorStatus.hasData
        ? "This sensor is reporting invalid data in the current-hour raw preview window."
        : "This sensor is offline, so no raw preview data is available.";
    }

    if (selectedSensorStatus.status === "idle" || !selectedSensorStatus.hasData) {
      return "No recent raw samples were found in the current-hour preview window. Use Export for long-range analysis.";
    }

    return "No raw preview data points matched the selected window. Use Export for long-range analysis.";
  }, [selectedNode, selectedSensorStatus, plotStatus]);

  return (
    <div className="sm-page">
      <div className="sm-toolbar">
        <div>
          <h1 className="sm-page-title">Sensor Management</h1>
          {nodesStatus && <p className="sm-inline-status">{nodesStatus}</p>}
          {settingsStatus && (
            <p className="sm-inline-status">{settingsStatus}</p>
          )}
          {faultStatus && <p className="sm-inline-status">{faultStatus}</p>}
          {!isAdmin && (
            <p className="sm-inline-status">
              Viewer access: configuration controls are disabled.
            </p>
          )}
        </div>
      </div>

      {!selectedNode ? (
        <div className="sm-empty-state">
          <h2>No node selected</h2>
          <p>
            Select a node from the table to inspect sensors, plots, and
            configuration.
          </p>
        </div>
      ) : (
        <div className="sm-layout">
          <NodeTable
            nodes={nodes}
            selectedNodeLabel={selectedNodeLabel}
            onSelectNode={setSelectedNodeLabel}
            faultCountsBySerial={allFaultCountsBySerial}
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
              onReset={() => handleNodeControl("reset")}
              disabled={
                !selectedNode ||
                sensor !== "accelerometer" ||
                !isAdmin
              }
            />

            <div className="sm-panel sm-full-width-panel">
              <div className="sm-plot-toolbar">
                <div>
                  <h2 className="sm-panel-title">
                    {selectedSensorDef.label} Raw Live Preview
                  </h2>
                  <p className="sm-panel-subtitle">
                    Current-hour raw preview for {selectedNode.serial}. Use Export for long-range analysis.
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
                      {timeframeOptions.map((option) => (
                        <option key={option.minutes} value={option.minutes}>
                          {option.label}
                        </option>
                      ))}
                    </select>
                  </div>

                  <div className="sm-plot-actions">
                    <span className="sm-last-updated">
                      {plotLastUpdated
                        ? `Updated ${plotLastUpdated.toLocaleTimeString()}`
                        : "Not updated yet"}
                    </span>

                    <button
                      type="button"
                      className="sm-refresh-button"
                      onClick={handleRefreshPlot}
                    >
                      Refresh
                    </button>
                  </div>
                </div>
              </div>

              <div className="sm-plot-status">{plotStatus}</div>
              <div className="sm-plot-note">
                Raw live preview is limited to short windows from the current hour file.
              </div>

              <div className="sm-plot-wrap">
                {apiData && selectedNode && hasPlotPoints(apiData) ? (
                  <SensorLineChart
                    title={`${selectedSensorDef.label} (${selectedNode.serial})`}
                    data={apiData}
                    height={420}
                  />
                ) : (
                  <div className="sm-plot-empty">
                    {plotEmptyMessage}
                  </div>
                )}
              </div>
            </div>

            <FaultLog
              variant="node"
              serial_number={selectedNode?.serial ?? ""}
              limit={5}
              previewMode={UI_PREVIEW_MODE}
              previewFaults={PREVIEW_FAULTS_BY_SERIAL[selectedNode?.serial ?? ""] ?? []}
            />
          </section>
        </div>
      )}
    </div>
  );
}