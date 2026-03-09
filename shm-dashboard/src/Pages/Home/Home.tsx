import { useEffect, useState } from "react";

import FaultLog from "../../components/FaultLog/Log";
import NodeMap from "../../components/NodeMap/NodeMap";
import { getHealth, getStorage } from "../../services/api";

import "./Home.css";

type StorageRes = {
  total_gb?: number;
  used_gb?: number;
  free_gb?: number;
  usage_percent?: number;
};

export default function Home() {
  const [backendOnline, setBackendOnline] = useState<boolean>(false);
  const [backendStatusText, setBackendStatusText] = useState<string>("Loading…");
  const [backendTime, setBackendTime] = useState<string>("");

  const [storageUsed, setStorageUsed] = useState<number>(0);
  const [storageFree, setStorageFree] = useState<number>(0);
  const [storageTotal, setStorageTotal] = useState<number>(0);
  const [storagePercent, setStoragePercent] = useState<number>(0);

  useEffect(() => {
    let mounted = true;

    async function pollHealth() {
      try {
        const res = await getHealth();
        if (!mounted) return;

        setBackendOnline(true);
        setBackendStatusText(res.status ?? "OK");
        setBackendTime(res.time ?? "");
      } catch (e) {
        if (!mounted) return;

        setBackendOnline(false);
        setBackendStatusText("Offline");
        setBackendTime("");
      }
    }

    async function pollStorage() {
      try {
        const res: StorageRes = await getStorage();
        if (!mounted) return;

        setStorageUsed(res.used_gb ?? 0);
        setStorageFree(res.free_gb ?? 0);
        setStorageTotal(res.total_gb ?? 0);
        setStoragePercent(res.usage_percent ?? 0);
      } catch (e) {
        if (!mounted) return;

        setStorageUsed(0);
        setStorageFree(0);
        setStorageTotal(0);
        setStoragePercent(0);
      }
    }

    // initial fetch
    pollHealth();
    pollStorage();

    // Poll every 5 seconds
    const id = window.setInterval(() => {
      pollHealth();
      pollStorage();
    }, 5000);

    return () => {
      mounted = false;
      window.clearInterval(id);
    };
  }, []);

  return (
    <div className="sc-page home-page">
      <div className="home-grid">
        <div className="sc-card">
          <div className="sc-card-title">Backend Status</div>
          <div className="sc-card-body">
            <div className="backend-status-row">
              <span className={`status-pill ${backendOnline ? "info" : "high"}`}>
                {backendOnline ? "Online" : "Offline"}
              </span>
              <span className="backend-status-text">{backendStatusText}</span>
            </div>

            {backendTime && (
              <div className="backend-time">
                <span className="mono">{backendTime}</span>
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
          </div>
        </div>

        <div className="sc-card node-health-card">
          <div className="sc-card-title">Node Placement</div>
          <div className="sc-card-body">
            <NodeMap nodeCount={3} />{" "}
            {/* Needs to change to be dynamic instead of hard coded */}
          </div>
        </div>

        <div className="sc-card home-faults-card">
          <div className="sc-card-title">Recent Faults</div>
          <div className="sc-card-body">
            <FaultLog limit={10} />
          </div>
        </div>
      </div>
    </div>
  );
}