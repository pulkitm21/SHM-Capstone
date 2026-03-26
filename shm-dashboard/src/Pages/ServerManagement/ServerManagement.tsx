import { useCallback, useEffect, useMemo, useState } from "react";
import {
  getHealth,
  getServerNetwork,
  getServerStatus,
  getStorage,
  getStorageStatus,
  pruneStoredData,
  rebootServer,
  renewVpnCertificate,
  restartBackendService,
  restartMqttService,
  unmountStorage,
  type HealthResponse,
  type ServerActionResponse,
  type ServerNetworkResponse,
  type ServerStatusResponse,
  type StorageResponse,
  type StorageStatusResponse,
} from "../../services/api";
import "./ServerManagement.css";

function getErrorMessage(error: unknown, fallback: string) {
  if (error instanceof Error && error.message) return error.message;
  return fallback;
}

function formatGb(value: unknown) {
  const num = Number(value);
  return Number.isFinite(num) ? `${num.toFixed(2)} GB` : "Unavailable";
}

function formatDateTime(value: unknown) {
  if (!value) return "Unavailable";
  const date = new Date(String(value));
  if (Number.isNaN(date.getTime())) return String(value);
  return date.toLocaleString();
}

function formatBooleanStatus(
  value: unknown,
  positive = "Healthy",
  negative = "Unavailable"
) {
  return Boolean(value) ? positive : negative;
}

function formatUptime(totalSeconds: unknown) {
  const seconds = Number(totalSeconds);
  if (!Number.isFinite(seconds) || seconds < 0) return "Unavailable";

  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);

  const parts: string[] = [];
  if (days > 0) parts.push(`${days}d`);
  if (hours > 0 || days > 0) parts.push(`${hours}h`);
  parts.push(`${minutes}m`);

  return parts.join(" ");
}

type BackendHealthBadgeState = "OK" | "DEGRADED" | "OFFLINE";

function getBackendStateFromSources(
  serverStatus: ServerStatusResponse | null,
  health: HealthResponse | null
): BackendHealthBadgeState {
  const statusValue = String(
    serverStatus?.backend_status ?? health?.status ?? "OFFLINE"
  ).toUpperCase();

  if (statusValue === "OK") return "OK";
  if (statusValue === "DEGRADED") return "DEGRADED";
  return "OFFLINE";
}

function getPillClass(state: BackendHealthBadgeState) {
  if (state === "OK") return "server-pill ok";
  if (state === "DEGRADED") return "server-pill warning";
  return "server-pill danger";
}

function getPillLabel(state: BackendHealthBadgeState) {
  if (state === "OK") return "Online";
  if (state === "DEGRADED") return "Degraded";
  return "Offline";
}

export default function ServerManagementPage() {
  const [health, setHealth] = useState<HealthResponse | null>(null);
  const [serverStatus, setServerStatus] =
    useState<ServerStatusResponse | null>(null);
  const [serverNetwork, setServerNetwork] =
    useState<ServerNetworkResponse | null>(null);
  const [storage, setStorage] = useState<StorageResponse | null>(null);
  const [storageStatus, setStorageStatus] =
    useState<StorageStatusResponse | null>(null);

  const [loading, setLoading] = useState(true);
  const [pageError, setPageError] = useState("");
  const [actionMessage, setActionMessage] = useState("");
  const [runningAction, setRunningAction] = useState("");

  const [retentionDays, setRetentionDays] = useState<number>(30);

  // Load all server management data used by this page.
  const loadPageData = useCallback(async (signal?: AbortSignal) => {
    const results = await Promise.allSettled([
      getHealth(signal),
      getServerStatus(signal),
      getServerNetwork(signal),
      getStorage(signal),
      getStorageStatus(signal),
    ]);

    const errors: string[] = [];

    const [
      healthResult,
      statusResult,
      networkResult,
      storageResult,
      storageStatusResult,
    ] = results;

    if (healthResult.status === "fulfilled") {
      setHealth(healthResult.value);
    } else {
      setHealth(null);
      errors.push("Health endpoint unavailable.");
    }

    if (statusResult.status === "fulfilled") {
      setServerStatus(statusResult.value);
    } else {
      setServerStatus(null);
      errors.push("Server status endpoint unavailable.");
    }

    if (networkResult.status === "fulfilled") {
      setServerNetwork(networkResult.value);
    } else {
      setServerNetwork(null);
      errors.push("Network endpoint unavailable.");
    }

    if (storageResult.status === "fulfilled") {
      setStorage(storageResult.value);
    } else {
      setStorage(null);
      errors.push("Storage endpoint unavailable.");
    }

    if (storageStatusResult.status === "fulfilled") {
      setStorageStatus(storageStatusResult.value);
    } else {
      setStorageStatus(null);
      errors.push("Storage status endpoint unavailable.");
    }

    setPageError(errors.join(" "));
  }, []);

  // Initial load plus periodic refresh for dashboard state.
  useEffect(() => {
    const controller = new AbortController();
    let mounted = true;

    async function load() {
      try {
        setLoading(true);
        await loadPageData(controller.signal);
      } finally {
        if (mounted) setLoading(false);
      }
    }

    void load();

    const intervalId = window.setInterval(() => {
      void loadPageData(controller.signal);
    }, 10000);

    return () => {
      mounted = false;
      controller.abort();
      window.clearInterval(intervalId);
    };
  }, [loadPageData]);

  const backendState = useMemo(
    () => getBackendStateFromSources(serverStatus, health),
    [serverStatus, health]
  );

  const storagePercent = Number(storage?.usage_percent ?? 0);
  const mqttHealthy = Boolean(serverStatus?.mqtt_connected ?? health?.mqtt);
  const faultDbHealthy = Boolean(
    serverStatus?.fault_db_available ?? health?.fault_db
  );
  const ssdHealthy = Boolean(
    serverStatus?.ssd_available ?? health?.ssd ?? storageStatus?.available
  );

  // Shared handler for server-side actions with optional confirmation.
  async function runServerAction(
    actionKey: string,
    actionLabel: string,
    actionFn: () => Promise<ServerActionResponse>,
    confirmMessage?: string
  ) {
    if (confirmMessage && !window.confirm(confirmMessage)) return;

    try {
      setRunningAction(actionKey);
      setActionMessage("");
      setPageError("");

      const response = await actionFn();

      setActionMessage(
        response.message ||
          response.detail ||
          `${actionLabel} request accepted.`
      );

      await loadPageData();
    } catch (error) {
      setPageError(getErrorMessage(error, `${actionLabel} failed.`));
    } finally {
      setRunningAction("");
    }
  }

  // Dedicated unmount flow because it uses a custom confirmation message.
  async function handleUnmountStorage() {
    const confirmed = window.confirm(
      "This will unmount the SSD and stop data storage until it is mounted again. Continue?"
    );
    if (!confirmed) return;

    try {
      setRunningAction("unmount-storage");
      setActionMessage("");
      setPageError("");

      const response = await unmountStorage();
      setActionMessage(
        response.message ||
          response.detail ||
          "Storage unmount request accepted."
      );

      await loadPageData();
    } catch (error) {
      setPageError(getErrorMessage(error, "Failed to unmount storage."));
    } finally {
      setRunningAction("");
    }
  }

  // Delete raw data files older than the selected retention window.
  async function handlePruneData() {
    if (!Number.isFinite(retentionDays) || retentionDays < 1) {
      setPageError("Retention days must be at least 1.");
      return;
    }

    const confirmed = window.confirm(
      `Delete stored data files older than ${retentionDays} day(s)? This action cannot be undone.`
    );
    if (!confirmed) return;

    await runServerAction(
      "prune-data",
      "Delete old data",
      () => pruneStoredData({ older_than_days: retentionDays }),
      undefined
    );
  }

  return (
    <div className="server-page">
      <section className="server-hero">
        <div>
          <h1 className="server-title">Server Management</h1>
        </div>

        <div className="server-hero-actions">
          <button
            type="button"
            className="server-action-btn"
            onClick={() => void loadPageData()}
            disabled={loading || !!runningAction}
          >
            Refresh
          </button>
        </div>
      </section>

      {pageError ? (
        <div className="server-alert server-alert-warning">{pageError}</div>
      ) : null}

      {actionMessage ? (
        <div className="server-alert server-alert-success">
          {actionMessage}
        </div>
      ) : null}

      <section className="server-grid">
        <article className="server-card server-card-span-2">
          <div className="server-card-header">
            <div>
              <p className="server-card-kicker">Overview</p>
              <h2 className="server-card-title">System Status</h2>
            </div>

            <span className={getPillClass(backendState)}>
              {getPillLabel(backendState)}
            </span>
          </div>

          <div className="server-metric-grid">
            <div className="server-metric">
              <span className="server-metric-label">Backend</span>
              <strong className="server-metric-value">
                {getPillLabel(backendState)}
              </strong>
            </div>

            <div className="server-metric">
              <span className="server-metric-label">MQTT</span>
              <strong className="server-metric-value">
                {formatBooleanStatus(mqttHealthy)}
              </strong>
            </div>

            <div className="server-metric">
              <span className="server-metric-label">Fault DB</span>
              <strong className="server-metric-value">
                {formatBooleanStatus(faultDbHealthy)}
              </strong>
            </div>

            <div className="server-metric">
              <span className="server-metric-label">SSD</span>
              <strong className="server-metric-value">
                {formatBooleanStatus(ssdHealthy)}
              </strong>
            </div>

            <div className="server-metric">
              <span className="server-metric-label">Uptime</span>
              <strong className="server-metric-value">
                {formatUptime(serverStatus?.uptime_seconds)}
              </strong>
            </div>

            <div className="server-metric">
              <span className="server-metric-label">Last Boot</span>
              <strong className="server-metric-value">
                {formatDateTime(serverStatus?.last_boot)}
              </strong>
            </div>
          </div>

          <div className="server-detail-list">
            <div className="server-detail-row">
              <span className="server-detail-label">Last Health Update</span>
              <span className="server-detail-value">
                {formatDateTime(serverStatus?.time ?? health?.time)}
              </span>
            </div>
          </div>
        </article>

        <article className="server-card">
          <div className="server-card-header">
            <div>
              <p className="server-card-kicker">Storage</p>
              <h2 className="server-card-title">Storage Management</h2>
            </div>
          </div>

          <div className="server-storage-stats">
            <div className="server-storage-stat">
              <span className="server-storage-label">Used</span>
              <strong>{formatGb(storage?.used_gb)}</strong>
            </div>

            <div className="server-storage-stat">
              <span className="server-storage-label">Free</span>
              <strong>{formatGb(storage?.free_gb)}</strong>
            </div>

            <div className="server-storage-stat">
              <span className="server-storage-label">Total</span>
              <strong>{formatGb(storage?.total_gb)}</strong>
            </div>
          </div>

          <div className="server-storage-bar">
            <div
              className="server-storage-fill"
              style={{ width: `${Math.max(0, Math.min(100, storagePercent))}%` }}
            />
          </div>

          <p className="server-storage-percent">
            {Number.isFinite(storagePercent)
              ? `${storagePercent.toFixed(1)}% used`
              : "Usage unavailable"}
          </p>

          <div className="server-detail-list">
            <div className="server-detail-row">
              <span className="server-detail-label">Mount Path</span>
              <span className="server-detail-value">
                {String(storageStatus?.mount_path ?? "/mnt/ssd")}
              </span>
            </div>

            <div className="server-detail-row">
              <span className="server-detail-label">Mounted</span>
              <span className="server-detail-value">
                {formatBooleanStatus(
                  storageStatus?.mounted,
                  "Mounted",
                  "Not mounted"
                )}
              </span>
            </div>

            <div className="server-detail-row">
              <span className="server-detail-label">Readable</span>
              <span className="server-detail-value">
                {formatBooleanStatus(
                  storageStatus?.readable,
                  "Readable",
                  "Not readable"
                )}
              </span>
            </div>

            <div className="server-detail-row">
              <span className="server-detail-label">Writable</span>
              <span className="server-detail-value">
                {formatBooleanStatus(
                  storageStatus?.writable,
                  "Writable",
                  "Not writable"
                )}
              </span>
            </div>
          </div>

          <div className="server-button-row">
            <button
              type="button"
              className="server-action-btn server-action-btn-danger"
              onClick={() => void handleUnmountStorage()}
              disabled={runningAction === "unmount-storage"}
            >
              {runningAction === "unmount-storage"
                ? "Unmounting..."
                : "Unmount SSD"}
            </button>
          </div>
        </article>

        <article className="server-card">
          <div className="server-card-header">
            <div>
              <p className="server-card-kicker">Connectivity</p>
              <h2 className="server-card-title">Network</h2>
            </div>
          </div>

          <div className="server-detail-list">
            <div className="server-detail-row">
              <span className="server-detail-label">VPN Status</span>
              <span className="server-detail-value">
                {formatBooleanStatus(
                  serverNetwork?.vpn_connected,
                  "Connected",
                  "Disconnected"
                )}
              </span>
            </div>

            <div className="server-detail-row">
              <span className="server-detail-label">Internet Reachable</span>
              <span className="server-detail-value">
                {formatBooleanStatus(
                  serverNetwork?.internet_reachable,
                  "Reachable",
                  "Unavailable"
                )}
              </span>
            </div>

            <div className="server-detail-row">
              <span className="server-detail-label">VPN Certificate Expiry</span>
              <span className="server-detail-value">
                {formatDateTime(serverNetwork?.vpn_cert_expires_at)}
              </span>
            </div>
          </div>
        </article>

        <article className="server-card">
          <div className="server-card-header">
            <div>
              <p className="server-card-kicker">Controls</p>
              <h2 className="server-card-title">Server Controls</h2>
            </div>
          </div>

          <div className="server-button-stack">
            <button
              type="button"
              className="server-action-btn"
              onClick={() =>
                void runServerAction(
                  "restart-backend",
                  "Restart backend",
                  restartBackendService,
                  "Restart the backend service now?"
                )
              }
              disabled={runningAction === "restart-backend"}
            >
              {runningAction === "restart-backend"
                ? "Restarting..."
                : "Restart Backend"}
            </button>

            <button
              type="button"
              className="server-action-btn"
              onClick={() =>
                void runServerAction(
                  "restart-mqtt",
                  "Restart MQTT",
                  restartMqttService,
                  "Restart the MQTT broker service now?"
                )
              }
              disabled={runningAction === "restart-mqtt"}
            >
              {runningAction === "restart-mqtt"
                ? "Restarting..."
                : "Restart MQTT"}
            </button>

            <button
              type="button"
              className="server-action-btn"
              onClick={() =>
                void runServerAction(
                  "renew-vpn",
                  "Renew VPN certificate",
                  renewVpnCertificate,
                  "Renew the VPN certificate now?"
                )
              }
              disabled={runningAction === "renew-vpn"}
            >
              {runningAction === "renew-vpn"
                ? "Renewing..."
                : "Renew VPN Certificate"}
            </button>

            <button
              type="button"
              className="server-action-btn server-action-btn-danger"
              onClick={() =>
                void runServerAction(
                  "reboot-server",
                  "Reboot server",
                  rebootServer,
                  "Reboot the Raspberry Pi now? This will disconnect the dashboard temporarily."
                )
              }
              disabled={runningAction === "reboot-server"}
            >
              {runningAction === "reboot-server" ? "Rebooting..." : "Reboot Pi"}
            </button>
          </div>
        </article>

        <article className="server-card server-card-span-2">
          <div className="server-card-header">
            <div>
              <p className="server-card-kicker">Maintenance</p>
              <h2 className="server-card-title">Maintenance Tools</h2>
            </div>
          </div>

          <div className="server-maintenance-grid">
            <div className="server-maintenance-panel">
              <h3 className="server-panel-title">Delete Old Data</h3>
              <p className="server-panel-text">
                Remove stored data files older than the selected retention
                window.
              </p>

              <div className="server-inline-form">
                <label className="server-field">
                  <span className="server-field-label">Retention (days)</span>
                  <input
                    type="number"
                    min={1}
                    step={1}
                    value={retentionDays}
                    onChange={(event) =>
                      setRetentionDays(Number(event.target.value))
                    }
                    className="server-input"
                  />
                </label>

                <button
                  type="button"
                  className="server-action-btn server-action-btn-danger"
                  onClick={() => void handlePruneData()}
                  disabled={runningAction === "prune-data"}
                >
                  {runningAction === "prune-data"
                    ? "Deleting..."
                    : "Delete Old Data"}
                </button>
              </div>
            </div>
          </div>
        </article>
      </section>
    </div>
  );
}