import { useEffect, useState } from "react";
import { getFaults } from "../../services/api";
import "./FaultLog.css";

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

type FaultEventsResponse = {
  faults?: FaultEntry[];
  time?: string;
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
    let mounted = true;
    let eventSource: EventSource | null = null;
    let reconnectTimeoutId: number | null = null;

    async function loadFaultsFallback() {
      try {
        const res = await getFaults({ serial_number, limit });
        if (!mounted) return;

        setFaults(res.faults ?? []);
        setStatus("");
      } catch (err: any) {
        console.error(err);
        if (!mounted) return;

        setFaults([]);
        setStatus("Fault log load failed");
      }
    }

    function connectFaultLogSSE() {
      const params = new URLSearchParams();
      if (serial_number) params.set("serial_number", serial_number);
      params.set("limit", String(limit));

      const query = params.toString();

      // SSE code for live fault log updates on the frontend dashboard.
      eventSource = new EventSource(
        `${import.meta.env.VITE_API_BASE_URL}/api/events/faults${query ? `?${query}` : ""}`
      );

      eventSource.onopen = () => {
        if (!mounted) return;
        setStatus("Loading…");
      };

      eventSource.onmessage = (event) => {
        if (!mounted) return;

        try {
          const data: FaultEventsResponse = JSON.parse(event.data);
          setFaults(data.faults ?? []);
          setStatus("");
        } catch (err) {
          console.error(err);
          setStatus("Fault log load failed");
        }
      };

      eventSource.onerror = () => {
        if (!mounted) return;

        eventSource?.close();
        setStatus("Fault log connection lost — retrying…");

        reconnectTimeoutId = window.setTimeout(() => {
          if (mounted) {
            void loadFaultsFallback();
            connectFaultLogSSE();
          }
        }, 3000);
      };
    }

    connectFaultLogSSE();

    return () => {
      mounted = false;
      eventSource?.close();
      if (reconnectTimeoutId !== null) {
        window.clearTimeout(reconnectTimeoutId);
      }
    };
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