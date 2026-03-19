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
    } catch {}
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

export type SetAccelerometerHpfBody = {
  highPassFilterDesired: "none" | "on";
};

export type SetAccelerometerHpfResponse = {
  node_id: number;
  serial: string;
  sensor: "accelerometer";
  desired: {
    highPassFilter: "none" | "on";
  };
  applied: {
    highPassFilter: "none" | "on" | null;
  };
  sync_status: "unknown" | "synced" | "pending" | "failed";
  request_id?: string;
  acked_at?: string | null;
};

export function getHealth(signal?: AbortSignal) {
  // Testing/manual health check endpoint only. SSE is used for backend status updates in the dashboard.
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

export function putAccelerometerHpf(
  nodeId: number,
  body: SetAccelerometerHpfBody,
  signal?: AbortSignal
) {
  return request<SetAccelerometerHpfResponse>(`/api/nodes/${nodeId}/config/accelerometer/hpf`, {
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

export type FaultsResponse = {
  faults: FaultRow[];
  [key: string]: unknown;
};

export function getFaults(
  params?: { serial_number?: string; limit?: number },
  signal?: AbortSignal
) {
  const qs = new URLSearchParams();

  if (params?.serial_number) qs.set("serial_number", params.serial_number);
  if (params?.limit !== undefined) qs.set("limit", String(params.limit));

  const suffix = qs.toString() ? `?${qs.toString()}` : "";
  return request<FaultsResponse>(`/api/faults${suffix}`, { signal });
}