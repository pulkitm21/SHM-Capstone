// API Client 
// This file serves as the communcation later between the frontend and the backend 

//URL for Backend 
const API_BASE = import.meta.env.VITE_API_BASE_URL;

// Request Wrapper for fetch()
/** This function:
 * prefixes the base API URL
 * performs the http request
 * checks for errors
 * parses the JSON response
 * 
 * <T> allows typescript to enfore return type -> done for typen safety
 * */

async function request<T>(
  path: string,
  options?: RequestInit & { signal?: AbortSignal }
): Promise<T> {

  // HTTP request
  const res = await fetch(`${API_BASE}${path}`, options);

  // Throw and error if the response status is not a success (between 200–299)
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

//Type Definition for API Responses 
/**
 * Define expected strucutre of resposne improving type safety
 * */

//Sensor data point
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
export function getSettings(signal?: AbortSignal) {
  return request<SettingsResponse>("/api/settings", { signal });
}


// Updates sensor configuration on backend.
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
 *  minutes → timeframe window
 *  channel → channel identifier
 *
 * Returns:
 * - Structured ApiResponse object
 */
export function getSensorData(
  endpoint: string,
  params: { minutes: number; channel?: string },
  signal?: AbortSignal
) {
  const qs = new URLSearchParams();
  qs.set("minutes", String(params.minutes));
  if (params.channel) qs.set("channel", params.channel);

  return request<ApiResponse>(`${endpoint}?${qs.toString()}`, { signal });
}
