// API Client
// This file serves as the communcation later between the frontend and the backend

// URL for Backend
const API_BASE = import.meta.env.VITE_API_BASE_URL;

// Request Wrapper for fetch()
/** This function:
 * prefixes the base API URL
 * performs the http request
 * checks for errors
 * parses the JSON response
 *
 * <T> allows typescript to enfore return type -> done for type safety
 */
async function request<T>(
  path: string,
  options?: RequestInit & { signal?: AbortSignal }
): Promise<T> {
  // HTTP request
  const res = await fetch(`${API_BASE}${path}`, options);

  // Throw an error if the response status is not a success (between 200–299)
  // https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Status
  if (!res.ok) {
    let msg = `HTTP ${res.status}`;
    try {
      const text = await res.text();
      if (text) msg += ` - ${text}`;
    } catch {}
    throw new Error(msg);
  }

  // Parse and return JSON response
  return (await res.json()) as T;
}

// Type Definition for API Responses

// Sensor data point
export type SensorPoint = {
  t: string;
  v: number;
};

export type ApiResponse = {
  points: SensorPoint[];
  sensor?: string;
  unit?: string;

  // Allow backend to send extra properties without breaking TypeScript
  [key: string]: unknown;
};

export type SettingsResponse = {
  meta: Record<string, unknown>;
  config: Record<string, unknown>;

  [key: string]: unknown;
};

export type HealthResponse = {
  status?: string;
  time?: string;

  [key: string]: unknown;
};

// API Functions

// Used to verify backend connectivity.
export function getHealth(signal?: AbortSignal) {
  return request<HealthResponse>("/health", { signal });
}

// Fetches all sensor metadata + configuration from backend.
// (1 settings file for all nodes)
export function getSettings(signal?: AbortSignal) {
  return request<SettingsResponse>("/api/settings", { signal });
}

// Updates sensor configuration on backend.
// (1 settings file for all nodes)
export function putSettings(body: SettingsResponse, signal?: AbortSignal) {
  return request<SettingsResponse>("/api/settings", {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
    signal,
  });
}

/**
 * GET sensor data endpoint
 *
 * Parameters:
 *  endpoint → backend route
 *  node → node identifier (Option A)
 *  minutes → timeframe window
 *  channel → channel identifier
 *
 * Returns:
 * - Structured ApiResponse object
 */
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


/* --------------------------------------------------------------------------
   Fault Log API
   This section adds frontend support for retrieving fault logs from the
   backend SQLite database via the /api/faults endpoint.
   Supports optional filtering by node.
-------------------------------------------------------------------------- */

// Structure of a fault entry returned by backend
export type FaultRow = {
  id: number;
  ts: string;
  severity: "High" | "Warning" | "Info";
  node_id: number;
  sensor_id: string;
  fault_type: string;

  // Allow backend to add extra properties without breaking TypeScript
  [key: string]: unknown;
};

// Response wrapper for fault log API
export type FaultsResponse = {
  faults: FaultRow[];

  [key: string]: unknown;
};

// Fetch faults from backend
export function getFaults(
  params?: { node?: number; limit?: number },
  signal?: AbortSignal
) {
  const qs = new URLSearchParams();

  if (params?.node !== undefined) qs.set("node", String(params.node));
  if (params?.limit !== undefined) qs.set("limit", String(params.limit));

  const suffix = qs.toString() ? `?${qs.toString()}` : "";

  return request<FaultsResponse>(`/api/faults${suffix}`, { signal });
}

// Get storage information from backend
export async function getStorage() {
  const res = await fetch(`${API_BASE}/api/storage`);
  if (!res.ok) throw new Error("storage failed");
  return res.json();
}