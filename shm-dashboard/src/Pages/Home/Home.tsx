import { useEffect, useMemo, useState } from "react";
import { useNavigate } from "react-router-dom";

import FaultLog from "../../components/FaultLog/Log";
import NodeMap from "../../components/NodeMap/NodeMap";
import {
  getNodes,
  getStorage,
  getStorageStatus,
  type NodeRecord,
  type StorageResponse,
  type StorageStatusResponse,
} from "../../services/api";

import "./Home.css";

export default function Home() {
  const navigate = useNavigate();

  const [backendOnline, setBackendOnline] = useState<boolean>(false);
  const [lastOnline, setLastOnline] = useState<string>("");
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

  useEffect(() => {
    let mounted = true;
    let eventSource: EventSource | null = null;
    let reconnectTimeoutId: number | null = null;

    function formatLocalDateTime(value: string) {
      const date = new Date(value);
      if (Number.isNaN(date.getTime())) return value;
      return date.toLocaleString();
    }

    function connectBackendStatusSSE() {
      // SSE code for backend status live updates on the frontend dashboard.
      eventSource = new EventSource(
        `${import.meta.env.VITE_API_BASE_URL}/api/events/health`
      );

      eventSource.onopen = () => {
        if (!mounted) return;
        setBackendOnline(true);
      };

      eventSource.onmessage = (event) => {
        if (!mounted) return;

        try {
          const data = JSON.parse(event.data);
          const receivedAt = new Date().toLocaleString();
          const onlineTime = data.time ? formatLocalDateTime(data.time) : receivedAt;

          setBackendOnline(true);
          setLastOnline(onlineTime);
          setLastUpdate(receivedAt);
        } catch {
          const receivedAt = new Date().toLocaleString();

          setBackendOnline(true);
          setLastOnline(receivedAt);
          setLastUpdate(receivedAt);
        }
      };

      eventSource.onerror = () => {
        if (!mounted) return;

        setBackendOnline(false);

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

    connectBackendStatusSSE();
    pollNodes();
    pollStorage();
    pollStorageStatus();

    const id = window.setInterval(() => {
      pollNodes();
      pollStorage();
      pollStorageStatus();
    }, 5000);

    return () => {
      mounted = false;
      eventSource?.close();

      if (reconnectTimeoutId !== null) {
        window.clearTimeout(reconnectTimeoutId);
      }

      window.clearInterval(id);
    };
  }, []);

  // Count online nodes for quick dashboard context.
  const onlineNodeCount = useMemo(
    () => nodes.filter((node) => node.online).length,
    [nodes]
  );

  return (
    <div className="home-page">
      <div className="home-hero">
        <div>
          <h1 className="home-title">System Overview</h1>
        </div>
      </div>

      <div className="home-grid">
        <section className="sc-card home-card">
          <div className="home-card-header">
            <div>
              <p className="home-card-kicker">Connectivity</p>
              <div className="sc-card-title">Backend Status</div>
            </div>

            <span className={`status-pill ${backendOnline ? "info" : "high"}`}>
              {backendOnline ? "Online" : "Offline"}
            </span>
          </div>

          <div className="sc-card-body home-card-body">
            <div className="home-detail-list">
              <div className="home-detail-row">
                <span>Last Online</span>
                <span className="mono home-detail-value">
                  {lastOnline || "—"}
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
                  {onlineNodeCount}
                </span>
              </div>
            </div>
          </div>
        </section>

        <section className="sc-card home-card">
          <div className="home-card-header">
            <div>
              <p className="home-card-kicker">Storage</p>
              <div className="sc-card-title">SSD Health</div>
            </div>

            <span className={`status-pill ${ssdAvailable ? "info" : "high"}`}>
              {ssdAvailable ? "Available" : "Unavailable"}
            </span>
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
                style={{ width: `${Math.max(0, Math.min(100, storagePercent))}%` }}
              />
            </div>

            <div className="storage-percent">{storagePercent.toFixed(1)}% used</div>

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
              <p className="home-card-kicker">Nodes</p>
              <div className="sc-card-title">Node Placement</div>
            </div>

            <span className="home-map-hint">Drag to reposition</span>
          </div>

          <div className="sc-card-body home-card-body node-map-card-body">
            {nodesStatus ? (
              <p>{nodesStatus}</p>
            ) : (
              <NodeMap
                nodes={nodes}
                onNodeClick={(node) =>
                  navigate(`/sensor-management?serial=${encodeURIComponent(node.serial)}`)
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
            <FaultLog variant="recent" limit={5} />
          </div>

          <div className="home-faults-footer">
            <button
              className="home-link-btn"
              onClick={() => navigate("/fault-log")}
              type="button"
            >
              View all faults
            </button>
          </div>
        </section>
      </div>
    </div>
  );
}