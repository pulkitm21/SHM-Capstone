import type { FaultRow } from "../../services/api";

export function formatFaultTimestamp(ts?: string) {
  if (!ts) return "—";
  const date = new Date(ts);
  if (Number.isNaN(date.getTime())) return ts;
  return date.toLocaleString();
}

export function formatRelativeTime(ts?: string) {
  if (!ts) return "";

  const date = new Date(ts);
  const now = Date.now();
  const diffMs = now - date.getTime();

  if (Number.isNaN(date.getTime())) return "";

  const diffMin = Math.floor(diffMs / (1000 * 60));
  const diffHr = Math.floor(diffMin / 60);
  const diffDay = Math.floor(diffHr / 24);

  if (diffMin < 1) return "just now";
  if (diffMin < 60) return `${diffMin} min ago`;
  if (diffHr < 24) return `${diffHr} hr ago`;
  return `${diffDay} day${diffDay === 1 ? "" : "s"} ago`;
}

export function getSeverityClass(severity?: number) {
  if ((severity ?? 0) >= 3) return "critical";
  if ((severity ?? 0) === 2) return "warning";
  return "info";
}

export function getStatusClass(status?: string) {
  const normalized = String(status ?? "").toLowerCase();

  if (normalized === "active") return "active";
  if (normalized === "resolved") return "resolved";
  if (normalized === "acknowledged") return "acknowledged";
  return "unknown";
}

export function getFaultSummary(faults: FaultRow[]) {
  return {
    total: faults.length,
    active: faults.filter((f) => String(f.fault_status ?? "").toLowerCase() === "active").length,
    critical: faults.filter((f) => Number(f.severity ?? 0) >= 3).length,
    latest: faults[0] ?? null,
  };
}

export function toTitleCase(value?: string) {
  if (!value) return "—";
  return value
    .split(/[_\s-]+/)
    .filter(Boolean)
    .map((part) => part.charAt(0).toUpperCase() + part.slice(1))
    .join(" ");
}