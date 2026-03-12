import { useEffect, useState } from "react";
import { getFaults } from "../../services/api";
import "./Log.css";

type FaultEntry = {
  id: number;
  ts: string;
  severity: number;
  serial_number: string;
  sensor_type: string;
  fault_type: string;
  fault_status: string;
  description: string;
};

type FaultLogProps = {
  serial_number?: string;
  limit?: number;
};

function getSeverityLabel(severity: number) {
  if (severity >= 3) return "High";
  if (severity === 2) return "Warning";
  return "Info";
}

function getSeverityClass(severity: number) {
  if (severity >= 3) return "high";
  if (severity === 2) return "warning";
  return "info";
}

export default function FaultLog({ serial_number, limit = 200 }: FaultLogProps) {
  const [faults, setFaults] = useState<FaultEntry[]>([]);
  const [status, setStatus] = useState("Loading…");

  useEffect(() => {
    const controller = new AbortController();

    async function loadFaults() {
      try {
        setStatus("Loading…");

        const res = await getFaults({ serial_number, limit }, controller.signal);

        setFaults(res.faults ?? []);
        setStatus("");
      } catch (err: any) {
        console.error(err);
        setFaults([]);
        setStatus("Fault log load failed");
      }
    }

    loadFaults();
    return () => controller.abort();
  }, [serial_number, limit]);

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

          {faults.map((entry) => {
            const severityLabel = getSeverityLabel(entry.severity);
            const severityClass = getSeverityClass(entry.severity);

            return (
              <tr key={entry.id}>
                <td className="mono">{entry.ts}</td>

                <td>
                  <span className={`status-pill ${severityClass}`}>
                    {severityLabel}
                  </span>
                </td>

                <td>
                  {entry.serial_number} – {entry.sensor_type}
                </td>

                <td>
                  {entry.fault_type}
                  {entry.description ? `: ${entry.description}` : ""}
                  {entry.fault_status ? ` (${entry.fault_status})` : ""}
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
  );
}