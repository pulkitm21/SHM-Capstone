const API_BASE = import.meta.env.VITE_API_BASE_URL;

async function request<T>(
  path: string,
  options?: RequestInit & { signal?: AbortSignal }
): Promise<T> {
  const res = await fetch(`${API_BASE}${path}`, options);

  if (!res.ok) {
    let msg = `HTTP ${res.status}`;
    try {
      const text = await res.text();
      if (text) msg += ` - ${text}`;
    } catch {
      // Ignore response parse errors here and throw the HTTP status instead.
    }
    throw new Error(msg);
  }

  return (await res.json()) as T;
}

export type SensorPoint = {
  t: string;
  v: number;
};

export type ApiResponse = {
  points: SensorPoint[];
  sensor?: string;
  unit?: string;
  [key: string]: unknown;
};

export type SettingsResponse = {
  site_name?: string;
  meta: Record<string, unknown>;
  config: Record<string, unknown>;
  [key: string]: unknown;
};

export type SiteNameResponse = {
  site_name: string;
  ok?: boolean;
  [key: string]: unknown;
};

export type UpdateSiteNameRequest = {
  site_name: string;
};

export type HealthResponse = {
  status?: string;
  time?: string;
  [key: string]: unknown;
};

export type StorageResponse = {
  total_gb?: number;
  used_gb?: number;
  free_gb?: number;
  usage_percent?: number;
  [key: string]: unknown;
};

export type StorageStatusResponse = {
  mount_path?: string;
  exists?: boolean;
  mounted?: boolean;
  readable?: boolean;
  writable?: boolean;
  available?: boolean;
  status?: string;
  time?: string;
  [key: string]: unknown;
};

export type NodeRecord = {
  node_id: number;
  serial: string;
  label: string;
  first_seen: string;
  last_seen: string;
  online: boolean;
  x?: number;
  y?: number;
  position_zone?: string;
  [key: string]: unknown;
};

export type NodesResponse = {
  nodes: NodeRecord[];
  [key: string]: unknown;
};

export type NodeResponse = {
  node: NodeRecord;
  [key: string]: unknown;
};

export type UpdateNodePositionRequest = {
  x: number;
  y: number;
};

export type UpdateNodePositionResponse = {
  ok: boolean;
  node: NodeRecord;
  [key: string]: unknown;
};

export type BulkNodePositionItem = {
  node_id: number;
  x: number;
  y: number;
};

export type BulkNodePositionsRequest = {
  positions: BulkNodePositionItem[];
};

export type BulkNodePositionsResponse = {
  ok: boolean;
  nodes: NodeRecord[];
  [key: string]: unknown;
};

export type SystemActionResponse = {
  ok: boolean;
  action: string;
  status: string;
  time: string;
};

export function getHealth(signal?: AbortSignal) {
  return request<HealthResponse>("/health", { signal });
}

export function getStorage(signal?: AbortSignal) {
  return request<StorageResponse>("/api/storage", { signal });
}

export function getStorageStatus(signal?: AbortSignal) {
  return request<StorageStatusResponse>("/api/storage/status", { signal });
}

export function getNodes(signal?: AbortSignal) {
  return request<NodesResponse>("/api/nodes", { signal });
}

export function getNode(nodeId: number, signal?: AbortSignal) {
  return request<NodeResponse>(`/api/nodes/${nodeId}`, { signal });
}

export function putNodePosition(
  nodeId: number,
  body: UpdateNodePositionRequest,
  signal?: AbortSignal
) {
  return request<UpdateNodePositionResponse>(`/api/nodes/${nodeId}/position`, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
    signal,
  });
}

export function putNodePositions(
  body: BulkNodePositionsRequest,
  signal?: AbortSignal
) {
  return request<BulkNodePositionsResponse>("/api/nodes/positions", {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
    signal,
  });
}

export function getSettings(signal?: AbortSignal) {
  return request<SettingsResponse>("/api/settings", { signal });
}

export function putSettings(body: SettingsResponse, signal?: AbortSignal) {
  return request<SettingsResponse>("/api/settings", {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
    signal,
  });
}

export function getSiteName(signal?: AbortSignal) {
  return request<SiteNameResponse>("/api/settings/site-name", { signal });
}

export function putSiteName(
  body: UpdateSiteNameRequest,
  signal?: AbortSignal
) {
  return request<SiteNameResponse>("/api/settings/site-name", {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
    signal,
  });
}

export function getSensorData(
  endpoint: string,
  params: { node: number; minutes: number; channel?: string },
  signal?: AbortSignal
) {
  const qs = new URLSearchParams();
  qs.set("node", String(params.node));
  qs.set("minutes", String(params.minutes));
  if (params.channel) qs.set("channel", params.channel);

  return request<ApiResponse>(`${endpoint}?${qs.toString()}`, { signal });
}

export type FaultRow = {
  id: number;
  ts: string;
  serial_number: string;
  sensor_type: string;
  fault_type: string;
  severity: number;
  fault_status: string;
  description: string;
  [key: string]: unknown;
};

export type FaultFilterOptions = {
  sensor_types: string[];
  fault_types: string[];
  severities: number[];
  statuses: string[];
};

export type FaultsResponse = {
  faults: FaultRow[];
  page: number;
  page_size: number;
  total_items: number;
  total_pages: number;
  filter_options?: FaultFilterOptions;
  [key: string]: unknown;
};

export type FaultsQueryParams = {
  serial_number?: string;
  sensor_type?: string;
  fault_type?: string;
  severity?: number;
  fault_status?: string;
  description?: string;
  page?: number;
  page_size?: number;
  limit?: number;
};

export function getFaults(params?: FaultsQueryParams, signal?: AbortSignal) {
  const qs = new URLSearchParams();

  if (params?.serial_number) qs.set("serial_number", params.serial_number);
  if (params?.sensor_type) qs.set("sensor_type", params.sensor_type);
  if (params?.fault_type) qs.set("fault_type", params.fault_type);
  if (params?.severity !== undefined) qs.set("severity", String(params.severity));
  if (params?.fault_status) qs.set("fault_status", params.fault_status);
  if (params?.description) qs.set("description", params.description);
  if (params?.page !== undefined) qs.set("page", String(params.page));
  if (params?.page_size !== undefined) qs.set("page_size", String(params.page_size));
  if (params?.limit !== undefined) qs.set("limit", String(params.limit));

  const suffix = qs.toString() ? `?${qs.toString()}` : "";
  return request<FaultsResponse>(`/api/faults${suffix}`, { signal });
}

export type AccelerometerOdrIndex = 0 | 1 | 2;
export type AccelerometerRange = 1 | 2 | 3;

export type NodeState =
  | "unknown"
  | "idle"
  | "configured"
  | "recording"
  | "reconfig"
  | "error";

export type ControlCommand = "start" | "stop";

export type ApplyAccelerometerConfigBody = {
  odr_index: AccelerometerOdrIndex;
  range: AccelerometerRange;
  hpf_corner: number;
};

export type ApplyAccelerometerConfigResponse = {
  ok: boolean;
  node_id: number;
  serial: string;
  sensor: "accelerometer";
  desired: {
    odr_index: AccelerometerOdrIndex;
    range: AccelerometerRange;
    hpf_corner: number;
  };
  status: string;


  // ACK / SEQ fields intentionally removed from the frontend contract.
};

export type NodeControlBody = {
  cmd: ControlCommand;
};

export type NodeControlResponse = {
  ok: boolean;
  node_id: number;
  serial: string;
  cmd: ControlCommand;
  status: string;
};

export function applyAccelerometerConfig(
  nodeId: number,
  body: ApplyAccelerometerConfigBody,
  signal?: AbortSignal
) {
  return request<ApplyAccelerometerConfigResponse>(
    `/api/nodes/${nodeId}/config/accelerometer/apply`,
    {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
      signal,
    }
  );
}

export function sendNodeControl(
  nodeId: number,
  body: NodeControlBody,
  signal?: AbortSignal
) {
  return request<NodeControlResponse>(`/api/nodes/${nodeId}/control`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
    signal,
  });
}

export function rebootPi(signal?: AbortSignal) {
  return request<SystemActionResponse>("/api/system/reboot", {
    method: "POST",
    signal,
  });
}

export function unmountStorage(signal?: AbortSignal) {
  return request<SystemActionResponse>("/api/storage/unmount", {
    method: "POST",
    signal,
  });
}