import { useEffect, useState } from "react";
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
  const [backendStatusText, setBackendStatusText] = useState<string>("Loading…");
  const [backendTime, setBackendTime] = useState<string>("");
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
  const [ssdStatusText, setSsdStatusText] = useState<string>("Loading…");
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
          setBackendStatusText(data.status ?? "OK");
          setBackendTime(data.time ?? "");
          setLastOnline(onlineTime);
          setLastUpdate(receivedAt);
        } catch {
          const receivedAt = new Date().toLocaleString();

          setBackendOnline(true);
          setBackendStatusText("OK");
          setLastOnline(receivedAt);
          setLastUpdate(receivedAt);
        }
      };

      eventSource.onerror = () => {
        if (!mounted) return;

        setBackendOnline(false);
        setBackendStatusText("Offline");
        setBackendTime("");

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
        setSsdStatusText(String(res.status ?? "unknown"));
        setSsdMountPath(String(res.mount_path ?? "/mnt/ssd"));
      } catch {
        if (!mounted) return;

        setSsdMounted(false);
        setSsdAvailable(false);
        setSsdStatusText("Status unavailable");
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

  return (
    <div className="home-grid">
      <div className="sc-card">
        <div className="sc-card-title">Backend Status</div>

        <div className="sc-card-body">
          <div className="backend-status-row">
            <span className={`status-pill ${backendOnline ? "info" : "high"}`}>
              {backendOnline ? "Online" : "Offline"}
            </span>
          </div>

          <div style={{ marginTop: 8 }}>{backendStatusText}</div>

          {backendTime && (
            <div className="backend-time">
              <span className="mono">{backendTime}</span>
            </div>
          )}

          {lastOnline && (
            <div style={{ marginTop: 8 }}>
              Last Online: <span className="mono">{lastOnline}</span>
            </div>
          )}

          {lastUpdate && (
            <div style={{ marginTop: 4 }}>
              Last Update: <span className="mono">{lastUpdate}</span>
            </div>
          )}
        </div>
      </div>

      <div className="sc-card">
        <div className="sc-card-title">Storage</div>

        <div className="sc-card-body">
          <div className="storage-row">
            <div>Used: {storageUsed.toFixed(1)} GB</div>
            <div>Free: {storageFree.toFixed(1)} GB</div>
            <div>Total: {storageTotal.toFixed(1)} GB</div>
          </div>

          <div className="storage-bar">
            <div
              className="storage-fill"
              style={{ width: `${Math.max(0, Math.min(100, storagePercent))}%` }}
            />
          </div>

          <div className="storage-percent">
            {storagePercent.toFixed(1)}% used
          </div>

          <div style={{ marginTop: 12 }}>
            <div style={{ marginBottom: 6, fontWeight: 600 }}>
              SSD Mount Diagnostic
            </div>

            <div className="backend-status-row">
              <span className={`status-pill ${ssdAvailable ? "info" : "high"}`}>
                {ssdAvailable ? "Available" : "Unavailable"}
              </span>
            </div>

            <div style={{ marginTop: 8 }}>
              Mounted: <span className="mono">{ssdMounted ? "Yes" : "No"}</span>
            </div>

            <div style={{ marginTop: 4 }}>
              Path: <span className="mono">{ssdMountPath}</span>
            </div>

            <div style={{ marginTop: 4 }}>
              Status: <span className="mono">{ssdStatusText}</span>
            </div>
          </div>
        </div>
      </div>

      <div className="sc-card node-health-card">
        <div className="sc-card-title">Node Placement</div>

        <div className="sc-card-body">
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
      </div>

      <div className="sc-card home-faults-card">
        <div className="sc-card-title">Recent Faults</div>

        <div className="sc-card-body">
          <FaultLog limit={10} />
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
      </div>
    </div>
  );
}