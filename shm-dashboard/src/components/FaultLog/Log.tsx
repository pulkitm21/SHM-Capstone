import { useEffect, useState } from "react";
import { getFaults } from "../../services/api";
import "./Log.css";

// ---- Fault structure returned by backend
type FaultEntry = {
  id: number;
  ts: string;
  severity: "High" | "Warning" | "Info";
  node_id: number;
  sensor_id: string;
  fault_type: string;
};

// ---- Component props
type FaultLogProps = {
  node?: number;   // optional node filter (used on Home page)
  limit?: number;  // limit number of faults returned
};

export default function FaultLog({ node, limit = 200 }: FaultLogProps) {
  const [faults, setFaults] = useState<FaultEntry[]>([]);
  const [status, setStatus] = useState("Loading…");

  useEffect(() => {
    const controller = new AbortController();

    async function loadFaults() {
      try {
        setStatus("Loading…");

        const res = await getFaults(
          { node, limit },
          controller.signal
        );

        setFaults(res.faults ?? []);
        setStatus("");
      } catch (err: any) {
        console.error(err);
        setFaults([]);
        setStatus(`Fault log load failed`);
      }
    }

    loadFaults();

    return () => controller.abort();
  }, [node, limit]);

  return (
    <div className="fault-log">
      {status && <p>{status}</p>}

      <table className="fault-table">
        <thead>
          <tr>
            <th>Time</th>
            <th>Status</th>
            <th>Location</th>
            <th>Description</th>
          </tr>
        </thead>

        <tbody>
          {faults.length === 0 && !status && (
            <tr>
              <td colSpan={4}>No faults recorded</td>
            </tr>
          )}

          {faults.map((entry) => (
            <tr key={entry.id}>
              <td className="mono">{entry.ts}</td>

              <td>
                <span className={`status-pill ${entry.severity.toLowerCase()}`}>
                  {entry.severity}
                </span>
              </td>

              <td>
                Node {entry.node_id} – {entry.sensor_id}
              </td>

              <td>{entry.fault_type}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}