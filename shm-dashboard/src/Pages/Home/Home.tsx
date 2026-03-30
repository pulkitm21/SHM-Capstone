import { useEffect, useMemo, useState } from "react";
import { useNavigate } from "react-router-dom";

import useAuth from "../../Auth/useAuth";
import FaultLog from "../../components/FaultLog/Log";
import NodeMap from "../../components/NodeMap/NodeMap";
import {
  getFaultSummary,
  getNodes,
  getStorage,
  getStorageStatus,
  type HealthResponse,
  type NodeRecord,
  type StorageResponse,
  type StorageStatusResponse,
} from "../../services/api";

import "./Home.css";

type BackendHealthBadgeState = "ONLINE" | "DEGRADED" | "OFFLINE";

function getBackendPillClass(state: BackendHealthBadgeState) {
  if (state === "ONLINE") return "info";
  if (state === "DEGRADED") return "warn";
  return "high";
}

function getBackendBadgeLabel(state: BackendHealthBadgeState) {
  if (state === "ONLINE") return "Online";
  if (state === "DEGRADED") return "Degraded";
  return "Offline";
}

function formatSubsystemHealth(value: boolean) {
  return value ? "Healthy" : "Unavailable";
}

function getBackendWarningMessage(state: BackendHealthBadgeState) {
  if (state === "OFFLINE") {
    return "Backend unavailable. Live status, node updates, and storage data may be stale.";
  }

  if (state === "DEGRADED") {
    return "Backend reachable, but one or more backend subsystems are unavailable.";
  }

  return "";
}

export default function Home() {
  const { isAdmin } = useAuth();
  const navigate = useNavigate();

  const [backendHealthState, setBackendHealthState] =
    useState<BackendHealthBadgeState>("OFFLINE");
  const [mqttHealthy, setMqttHealthy] = useState<boolean>(false);
  const [faultDbHealthy, setFaultDbHealthy] = useState<boolean>(false);
  const [lastUpdate, setLastUpdate] = useState<string>("");

  const [nodes, setNodes] = useState<NodeRecord[]>([]);
  const [nodesStatus, setNodesStatus] = useState<string>("");

  const [storageUsed, setStorageUsed] = useState<number>(0);
  const [storageFree, setStorageFree] = useState<number>(0);
  const [storageTotal, setStorageTotal] = useState<number>(0);
  const [storagePercent, setStoragePercent] = useState<number>(0);

  const [ssdMounted, setSsdMounted] = useState<boolean>(false);
  const [ssdAvailable, setSsdAvailable] = useState<boolean>(false);
  const [ssdMountPath, setSsdMountPath] = useState<string>("/mnt/ssd");

  const [warningSerials, setWarningSerials] = useState<string[]>([]);

  useEffect(() => {
    let mounted = true;
    let eventSource: EventSource | null = null;
    let reconnectTimeoutId: number | null = null;
    let heartbeatTimeoutId: number | null = null;

    function clearReconnectTimeout() {
      if (reconnectTimeoutId !== null) {
        window.clearTimeout(reconnectTimeoutId);
        reconnectTimeoutId = null;
      }
    }

    function clearHeartbeatTimeout() {
      if (heartbeatTimeoutId !== null) {
        window.clearTimeout(heartbeatTimeoutId);
        heartbeatTimeoutId = null;
      }
    }

    function resetHeartbeatTimeout() {
      clearHeartbeatTimeout();

      heartbeatTimeoutId = window.setTimeout(() => {
        if (!mounted) return;

        setBackendHealthState("OFFLINE");
        setMqttHealthy(false);
        setFaultDbHealthy(false);
      }, 12000);
    }

    function connectBackendStatusSSE() {
      clearReconnectTimeout();
      eventSource?.close();

      // Use a same-origin path so local dev works through the Vite proxy.
      eventSource = new EventSource("/api/events/health");

      eventSource.onmessage = (event) => {
        if (!mounted) return;

        try {
          const data: HealthResponse = JSON.parse(event.data);
          const receivedAt = new Date().toLocaleString();

          // Distinguish between backend reachable but degraded vs truly offline.
          setBackendHealthState(
            data.status === "DEGRADED" ? "DEGRADED" : "ONLINE"
          );
          setMqttHealthy(Boolean(data.mqtt));
          setFaultDbHealthy(Boolean(data.fault_db));
          setLastUpdate(receivedAt);
          resetHeartbeatTimeout();
        } catch {
          const receivedAt = new Date().toLocaleString();

          setBackendHealthState("DEGRADED");
          setMqttHealthy(false);
          setFaultDbHealthy(false);
          setLastUpdate(receivedAt);
          resetHeartbeatTimeout();
        }
      };

      eventSource.onerror = () => {
        if (!mounted) return;

        setBackendHealthState("OFFLINE");
        setMqttHealthy(false);
        setFaultDbHealthy(false);

        clearHeartbeatTimeout();
        eventSource?.close();

        reconnectTimeoutId = window.setTimeout(() => {
          if (mounted) connectBackendStatusSSE();
        }, 3000);
      };
    }

    async function pollNodes() {
      try {
        const res = await getNodes();
        if (!mounted) return;

        setNodes(res.nodes ?? []);
        setNodesStatus("");
      } catch (e: any) {
        if (!mounted) return;

        setNodes([]);
        setNodesStatus(`Node load failed: ${e?.message ?? "Unknown error"}`);
      }
    }

    async function pollStorage() {
      try {
        const res: StorageResponse = await getStorage();
        if (!mounted) return;

        setStorageUsed(Number(res.used_gb ?? 0));
        setStorageFree(Number(res.free_gb ?? 0));
        setStorageTotal(Number(res.total_gb ?? 0));
        setStoragePercent(Number(res.usage_percent ?? 0));
      } catch {
        if (!mounted) return;

        setStorageUsed(0);
        setStorageFree(0);
        setStorageTotal(0);
        setStoragePercent(0);
      }
    }

    async function pollStorageStatus() {
      try {
        const res: StorageStatusResponse = await getStorageStatus();
        if (!mounted) return;

        setSsdMounted(Boolean(res.mounted));
        setSsdAvailable(Boolean(res.available));
        setSsdMountPath(String(res.mount_path ?? "/mnt/ssd"));
      } catch {
        if (!mounted) return;

        setSsdMounted(false);
        setSsdAvailable(false);
        setSsdMountPath("/mnt/ssd");
      }
    }

    async function pollFaultWarnings() {
      try {
        const res = await getFaultSummary();
        if (!mounted) return;

        setWarningSerials(res.warning_serials ?? []);
      } catch {
        if (!mounted) return;
        setWarningSerials([]);
      }
    }

    connectBackendStatusSSE();
    pollNodes();
    pollStorage();
    pollStorageStatus();
    pollFaultWarnings();

    const id = window.setInterval(() => {
      pollNodes();
      pollStorage();
      pollStorageStatus();
      pollFaultWarnings();
    }, 5000);

    return () => {
      mounted = false;
      eventSource?.close();
      clearReconnectTimeout();
      clearHeartbeatTimeout();
      window.clearInterval(id);
    };
  }, []);

  const onlineNodeCount = useMemo(
    () => nodes.filter((node) => node.online).length,
    [nodes]
  );

  const backendWarningMessage = getBackendWarningMessage(backendHealthState);
  const showBackendWarning = backendHealthState !== "ONLINE";

  return (
    <div className="home-page">
      <div className="home-hero">
        <div>
          <h1 className="home-title">System Overview</h1>

          {showBackendWarning && (
            <div className="home-warning-alert" role="status" aria-live="polite">
              <span className="home-warning-pill">Warning</span>
              <span className="home-warning-text">{backendWarningMessage}</span>
            </div>
          )}
        </div>
      </div>

      <div className="home-grid">
        <section className="sc-card home-card">
          <div className="home-card-header">
            <div>
              <div className="sc-card-title">Backend Status</div>
            </div>

            <div className="home-card-actions">
              <span className={`status-pill ${getBackendPillClass(backendHealthState)}`}>
                {getBackendBadgeLabel(backendHealthState)}
              </span>
            </div>
          </div>

          <div className="sc-card-body home-card-body">
            <div className="home-detail-list">
              <div className="home-detail-row">
                <span>MQTT Listener</span>
                <span className="mono home-detail-value">
                  {formatSubsystemHealth(mqttHealthy)}
                </span>
              </div>

              <div className="home-detail-row">
                <span>Fault DB</span>
                <span className="mono home-detail-value">
                  {formatSubsystemHealth(faultDbHealthy)}
                </span>
              </div>

              <div className="home-detail-row">
                <span>Last Update</span>
                <span className="mono home-detail-value">
                  {lastUpdate || "—"}
                </span>
              </div>

              <div className="home-detail-row">
                <span>Nodes Online</span>
                <span className="mono home-detail-value">
                  {onlineNodeCount} / {nodes.length}
                </span>
              </div>
            </div>
          </div>
        </section>

        <section className="sc-card home-card">
          <div className="home-card-header">
            <div>
              <div className="sc-card-title">SSD Health</div>
            </div>

            <div className="home-card-actions">
              <span className={`status-pill ${ssdAvailable ? "info" : "high"}`}>
                {ssdAvailable ? "Available" : "Unavailable"}
              </span>
            </div>
          </div>

          <div className="sc-card-body home-card-body">
            <div className="home-storage-stats">
              <div className="home-storage-stat">
                <span className="home-storage-label">Used</span>
                <strong>{storageUsed.toFixed(1)} GB</strong>
              </div>

              <div className="home-storage-stat">
                <span className="home-storage-label">Free</span>
                <strong>{storageFree.toFixed(1)} GB</strong>
              </div>

              <div className="home-storage-stat">
                <span className="home-storage-label">Total</span>
                <strong>{storageTotal.toFixed(1)} GB</strong>
              </div>
            </div>

            <div className="storage-bar">
              <div
                className="storage-fill"
                style={{
                  width: `${Math.max(0, Math.min(100, storagePercent))}%`,
                }}
              />
            </div>

            <div className="storage-percent">
              {storagePercent.toFixed(1)}% used
            </div>

            <div className="home-detail-list compact">
              <div className="home-detail-row">
                <span>Mounted</span>
                <span className="mono home-detail-value">
                  {ssdMounted ? "Yes" : "No"}
                </span>
              </div>

              <div className="home-detail-row">
                <span>Path</span>
                <span className="mono home-detail-value">{ssdMountPath}</span>
              </div>
            </div>
          </div>
        </section>

        <section className="sc-card home-card node-health-card">
          <div className="home-card-header">
            <div>
              <div className="sc-card-title">Node Placement</div>
            </div>
          </div>

          <div className="sc-card-body home-card-body node-map-card-body">
            {nodesStatus ? (
              <p>{nodesStatus}</p>
            ) : (
              <NodeMap
                nodes={nodes}
                warningSerials={warningSerials}
                canEdit={isAdmin}
                onNodeClick={(node) =>
                  navigate(
                    `/sensor-management?serial=${encodeURIComponent(node.serial)}`
                  )
                }
              />
            )}
          </div>
        </section>

        <section className="sc-card home-card home-faults-card">
          <div className="home-card-header">
            <div>
              <div className="sc-card-title">Recent Faults</div>
            </div>
          </div>

          <div className="sc-card-body home-card-body">
            <FaultLog variant="recent" limit={3} />
          </div>
        </section>
      </div>
    </div>
  );
}