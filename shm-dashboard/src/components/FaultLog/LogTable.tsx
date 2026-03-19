import type { FaultRow } from "../../services/api";
import {
  formatFaultTimestamp,
  getSeverityClass,
  getStatusClass,
  toTitleCase,
} from "./faultLog.utils";

type Props = {
  faults: FaultRow[];
  loading: boolean;
  error: string;
};

export default function LogTable({faults, loading, error }: Props) {
  return (
    <div className="faultlog-panel">
      <div className="faultlog-header">
        <span className="faultlog-count">{faults.length}</span>
      </div>

      {loading ? (
        <div className="faultlog-empty">Loading faults…</div>
      ) : error ? (
        <div className="faultlog-empty">Unable to load faults: {error}</div>
      ) : faults.length === 0 ? (
        <div className="faultlog-empty">No faults found.</div>
      ) : (
        <div className="faultlog-table-wrap">
          <table className="faultlog-table">
            <thead>
              <tr>
                <th>ID</th>
                <th>Timestamp</th>
                <th>Source</th>
                <th>Sensor</th>
                <th>Fault Type</th>
                <th>Severity</th>
                <th>Status</th>
                <th>Description</th>
              </tr>
            </thead>

            <tbody>
              {faults.map((fault) => (
                <tr key={fault.id}>
                  <td>{fault.id ?? "—"}</td>
                  <td>{formatFaultTimestamp(fault.ts)}</td>
                  <td>{fault.serial_number || "Backend"}</td>
                  <td>{toTitleCase(fault.sensor_type)}</td>
                  <td>{fault.fault_type || "—"}</td>
                  <td>
                    <span className={`faultlog-pill severity ${getSeverityClass(Number(fault.severity))}`}>
                      {fault.severity ?? "—"}
                    </span>
                  </td>
                  <td>
                    <span className={`faultlog-pill status ${getStatusClass(fault.fault_status)}`}>
                      {toTitleCase(fault.fault_status)}
                    </span>
                  </td>
                  <td className="faultlog-description-cell">
                    {fault.description || "—"}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </div>
  );
}