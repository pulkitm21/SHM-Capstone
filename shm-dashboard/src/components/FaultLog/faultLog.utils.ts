// Format a backend timestamp into a readable local date/time string.
export function formatFaultTimestamp(ts?: string) {
  if (!ts) return "—";

  const date = new Date(ts);
  if (Number.isNaN(date.getTime())) return ts;

  return date.toLocaleString();
}

// Convert a timestamp into a relative label like "5 min ago".
export function formatRelativeTime(ts?: string) {
  if (!ts) return "";

  const date = new Date(ts);
  if (Number.isNaN(date.getTime())) return "";

  const now = Date.now();
  const diffMs = now - date.getTime();
  const diffMin = Math.floor(diffMs / (1000 * 60));
  const diffHr = Math.floor(diffMin / 60);
  const diffDay = Math.floor(diffHr / 24);

  if (diffMin < 1) return "just now";
  if (diffMin < 60) return `${diffMin} min ago`;
  if (diffHr < 24) return `${diffHr} hr ago`;
  return `${diffDay} day${diffDay === 1 ? "" : "s"} ago`;
}

// Map severity values to CSS classes used by the pill styles.
export function getSeverityClass(severity?: number) {
  if ((severity ?? 0) >= 3) return "critical";
  if ((severity ?? 0) === 2) return "warning";
  return "info";
}

// Normalize backend status values into CSS class names.
export function getStatusClass(status?: string) {
  const normalized = String(status ?? "").toLowerCase();

  if (normalized === "active") return "active";
  if (normalized === "resolved") return "resolved";
  if (normalized === "acknowledged") return "acknowledged";
  return "unknown";
}

// Convert spaced text into title case for the UI.
export function toTitleCase(value?: string) {
  if (!value) return "—";

  return value
    .split(/[_\s-]+/)
    .filter(Boolean)
    .map((part) => part.charAt(0).toUpperCase() + part.slice(1))
    .join(" ");
}