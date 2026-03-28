const API_BASE = import.meta.env.VITE_API_BASE_URL;

/*
  All API requests include credentials so browser-managed auth cookies
  will be sent automatically once backend session auth is wired in.
*/
async function request<T>(
  path: string,
  options?: RequestInit & { signal?: AbortSignal }
): Promise<T> {
  const res = await fetch(`${API_BASE}${path}`, {
    credentials: "include",
    ...options,
  });

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

export type AuthRole = "admin" | "viewer";

export type AuthUser = {
  id: number;
  username: string;
  role: AuthRole;
  created_at?: string;
  updated_at?: string;
  last_login_at?: string | null;
};

export type LoginRequest = {
  username: string;
  password: string;
};

export type LoginResponse = {
  ok: boolean;
  user: AuthUser;
};

export type CurrentUserResponse = {
  authenticated: boolean;
  user: AuthUser | null;
};

export type LogoutResponse = {
  ok: boolean;
};

export type CreateUserRequest = {
  username: string;
  password: string;
  role: AuthRole;
};

export type CreateUserResponse = {
  ok: boolean;
  user: AuthUser;
};

export type UsersListResponse = {
  users: AuthUser[];
};

export type UpdateUserRoleRequest = {
  role: AuthRole;
};

export type UpdateUserRoleResponse = {
  ok: boolean;
  user: AuthUser;
};

export type ResetUserPasswordRequest = {
  password: string;
};

export type ResetUserPasswordResponse = {
  ok: boolean;
};

export type DeleteUserResponse = {
  ok: boolean;
};

function encodeUsernamePath(username: string) {
  return encodeURIComponent(username.trim());
}

export function login(body: LoginRequest, signal?: AbortSignal) {
  return request<LoginResponse>("/api/auth/login", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
    signal,
  });
}

export function logout(signal?: AbortSignal) {
  return request<LogoutResponse>("/api/auth/logout", {
    method: "POST",
    signal,
  });
}

export function getCurrentUser(signal?: AbortSignal) {
  return request<CurrentUserResponse>("/api/auth/me", { signal });
}

export function getUsers(signal?: AbortSignal) {
  return request<UsersListResponse>("/api/users", { signal });
}

export function createUser(body: CreateUserRequest, signal?: AbortSignal) {
  return request<CreateUserResponse>("/api/users", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
    signal,
  });
}

export function updateUserRole(
  username: string,
  body: UpdateUserRoleRequest,
  signal?: AbortSignal
) {
  return request<UpdateUserRoleResponse>(
    `/api/users/${encodeUsernamePath(username)}/role`,
    {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
      signal,
    }
  );
}

export function resetUserPassword(
  username: string,
  body: ResetUserPasswordRequest,
  signal?: AbortSignal
) {
  return request<ResetUserPasswordResponse>(
    `/api/users/${encodeUsernamePath(username)}/password`,
    {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
      signal,
    }
  );
}

export function deleteUser(username: string, signal?: AbortSignal) {
  return request<DeleteUserResponse>(
    `/api/users/${encodeUsernamePath(username)}`,
    {
      method: "DELETE",
      signal,
    }
  );
}

export type AccelerometerPlotPoint = {
  ts: string;
  x: number;
  y: number;
  z: number;
};

export type InclinometerPlotPoint = {
  ts: string;
  roll: number;
  pitch: number;
  yaw: number;
};

export type TemperaturePlotPoint = {
  ts: string;
  value: number;
};

export type AccelerometerPlotResponse = {
  sensor: "accelerometer";
  unit: "g";
  node: number;
  points: AccelerometerPlotPoint[];
};

export type InclinometerPlotResponse = {
  sensor: "inclinometer";
  unit: "deg";
  node: number;
  points: InclinometerPlotPoint[];
};

export type TemperaturePlotResponse = {
  sensor: "temperature";
  unit: "C";
  node: number;
  points: TemperaturePlotPoint[];
};

export type ApiResponse =
  | AccelerometerPlotResponse
  | InclinometerPlotResponse
  | TemperaturePlotResponse;

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
  status?: "OK" | "DEGRADED";
  time?: string;
  mqtt?: boolean;
  ssd?: boolean;
  fault_db?: boolean;
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
  time?: string;
  message?: string;
  detail?: string;
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
  params: { node: number; minutes: number },
  signal?: AbortSignal
) {
  const qs = new URLSearchParams();
  qs.set("node", String(params.node));
  qs.set("minutes", String(params.minutes));

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

export type FaultSummaryItem = {
  active_count: number;
  latest_active_ts?: string | null;
};

export type FaultSummaryResponse = {
  by_serial: Record<string, FaultSummaryItem>;
  warning_serials: string[];
  total_active_faults: number;
  time: string;
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

export function getFaultSummary(signal?: AbortSignal) {
  return request<FaultSummaryResponse>("/api/faults/summary", { signal });
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

export function unmountStorage(signal?: AbortSignal) {
  return request<SystemActionResponse>("/api/storage/unmount", {
    method: "POST",
    signal,
  });
}

export type FaultExportParams = {
  start_day?: string;
  end_day?: string;
  serial_number?: string;
  sensor_type?: string;
  fault_type?: string;
  severity?: number;
  fault_status?: string;
  description?: string;
};

function parseFilenameFromDisposition(headerValue: string | null): string | null {
  if (!headerValue) return null;

  const utf8Match = headerValue.match(/filename\*=UTF-8''([^;]+)/i);
  if (utf8Match?.[1]) {
    return decodeURIComponent(utf8Match[1]);
  }

  const basicMatch = headerValue.match(/filename="?([^"]+)"?/i);
  return basicMatch?.[1] ?? null;
}

export async function downloadFaultExport(
  params: FaultExportParams,
  signal?: AbortSignal
) {
  const qs = new URLSearchParams();

  if (params.start_day) qs.set("start_day", params.start_day);
  if (params.end_day) qs.set("end_day", params.end_day);
  if (params.serial_number) qs.set("serial_number", params.serial_number);
  if (params.sensor_type) qs.set("sensor_type", params.sensor_type);
  if (params.fault_type) qs.set("fault_type", params.fault_type);
  if (params.severity !== undefined) qs.set("severity", String(params.severity));
  if (params.fault_status) qs.set("fault_status", params.fault_status);
  if (params.description) qs.set("description", params.description);

  const suffix = qs.toString() ? `?${qs.toString()}` : "";
  const res = await fetch(`${API_BASE}/api/exports/faults${suffix}`, {
    credentials: "include",
    signal,
  });

  if (!res.ok) {
    let msg = `HTTP ${res.status}`;
    try {
      const text = await res.text();
      if (text) msg += ` - ${text}`;
    } catch {
      // ignore body parse issues and use HTTP status only
    }
    throw new Error(msg);
  }

  const blob = await res.blob();
  const filename =
    parseFilenameFromDisposition(res.headers.get("Content-Disposition")) ??
    "fault_export.csv";

  const objectUrl = window.URL.createObjectURL(blob);

  try {
    const link = document.createElement("a");
    link.href = objectUrl;
    link.download = filename;
    document.body.appendChild(link);
    link.click();
    link.remove();
  } finally {
    window.URL.revokeObjectURL(objectUrl);
  }
}

export type SensorExportParams = {
  node_ids: number[];
  start_day: string;
  end_day: string;
  start_hour?: string;
  end_hour?: string;
};

async function downloadFromEndpoint(
  path: string,
  signal?: AbortSignal
): Promise<void> {
  const res = await fetch(`${API_BASE}${path}`, {
    credentials: "include",
    signal,
  });

  if (!res.ok) {
    let msg = `HTTP ${res.status}`;
    try {
      const text = await res.text();
      if (text) msg += ` - ${text}`;
    } catch {
      // ignore
    }
    throw new Error(msg);
  }

  const blob = await res.blob();
  const filename =
    parseFilenameFromDisposition(res.headers.get("Content-Disposition")) ??
    "download.bin";

  const objectUrl = window.URL.createObjectURL(blob);

  try {
    const link = document.createElement("a");
    link.href = objectUrl;
    link.download = filename;
    document.body.appendChild(link);
    link.click();
    link.remove();
  } finally {
    window.URL.revokeObjectURL(objectUrl);
  }
}

export async function downloadSensorExport(
  params: SensorExportParams,
  signal?: AbortSignal
) {
  if (!params.node_ids.length) {
    throw new Error("At least one node must be selected.");
  }

  const qs = new URLSearchParams();
  qs.set("node_ids", params.node_ids.join(","));
  qs.set("start_day", params.start_day);
  qs.set("end_day", params.end_day);
  if (params.start_hour) qs.set("start_hour", params.start_hour);
  if (params.end_hour) qs.set("end_hour", params.end_hour);

  await downloadFromEndpoint(`/api/exports/sensor-data?${qs.toString()}`, signal);
}

export type ServerStatusResponse = {
  backend_status?: "OK" | "DEGRADED" | "OFFLINE";
  mqtt_connected?: boolean;
  fault_db_available?: boolean;
  ssd_available?: boolean;
  uptime_seconds?: number;
  last_boot?: string;
  time?: string;
  [key: string]: unknown;
};

export type ServerNetworkResponse = {
  vpn_connected?: boolean;
  vpn_cert_expires_at?: string;
  internet_reachable?: boolean;
  [key: string]: unknown;
};

export type ServerActionResponse = {
  ok: boolean;
  action: string;
  status: string;
  time?: string;
  message?: string;
  detail?: string;
  [key: string]: unknown;
};

export type PruneStoredDataBody = {
  older_than_days: number;
};

export type SensorHealthStatus = "online" | "offline" | "warning" | "idle";

export type SensorHealthDetail = {
  status: SensorHealthStatus;
  has_data: boolean;
  has_valid_data?: boolean;
  has_nan_data?: boolean;
  last_data_ts?: string | null;
  active_fault_count: number;
  active_faults: FaultRow[];
};

export type NodeSensorStatusResponse = {
  node_id: number;
  serial: string;
  node_online: boolean;
  window_seconds: number;
  time: string;
  sensors: {
    accelerometer: SensorHealthDetail;
    inclinometer: SensorHealthDetail;
    temperature: SensorHealthDetail;
  };
};

export function getServerStatus(signal?: AbortSignal) {
  return request<ServerStatusResponse>("/api/server/status", { signal });
}

export function getServerNetwork(signal?: AbortSignal) {
  return request<ServerNetworkResponse>("/api/server/network", { signal });
}

export function rebootServer(signal?: AbortSignal) {
  return request<ServerActionResponse>("/api/server/reboot", {
    method: "POST",
    signal,
  });
}

export function restartBackendService(signal?: AbortSignal) {
  return request<ServerActionResponse>("/api/server/restart-backend", {
    method: "POST",
    signal,
  });
}

export function restartMqttService(signal?: AbortSignal) {
  return request<ServerActionResponse>("/api/server/restart-mqtt", {
    method: "POST",
    signal,
  });
}

export function renewVpnCertificate(signal?: AbortSignal) {
  return request<ServerActionResponse>("/api/server/renew-vpn-certificate", {
    method: "POST",
    signal,
  });
}

export function pruneStoredData(
  body: PruneStoredDataBody,
  signal?: AbortSignal
) {
  return request<ServerActionResponse>("/api/server/prune-data", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
    signal,
  });
}

// Fetch per-sensor health for the selected node.
export function getNodeSensorStatus(
  nodeId: number,
  windowSeconds = 120,
  signal?: AbortSignal
) {
  const qs = new URLSearchParams();
  qs.set("window_seconds", String(windowSeconds));

  return request<NodeSensorStatusResponse>(
    `/api/nodes/${nodeId}/sensor-status?${qs.toString()}`,
    { signal }
  );
}