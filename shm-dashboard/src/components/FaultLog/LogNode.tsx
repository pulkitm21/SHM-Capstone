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

export default function LogNode({faults, loading, error }: Props) {
  return (
    <div className="faultlog-panel">
      <div className="faultlog-header">
        <h2 className="faultlog-title">Node Faults</h2>
        <span className="faultlog-count">{faults.length}</span>
      </div>

      {loading ? (
        <div className="faultlog-empty">Loading faults…</div>
      ) : error ? (
        <div className="faultlog-empty">Unable to load faults: {error}</div>
      ) : faults.length === 0 ? (
        <div className="faultlog-empty">No node faults found.</div>
      ) : (
        <div className="faultlog-node-list">
          {faults.map((fault) => (
            <div key={fault.id} className="faultlog-node-card">
              <div className="faultlog-node-top">
                <div className="faultlog-node-title">{fault.fault_type || "Unknown fault"}</div>

                <div className="faultlog-node-badges">
                  <span className={`faultlog-pill severity ${getSeverityClass(Number(fault.severity))}`}>
                    Severity {fault.severity ?? "—"}
                  </span>
                  <span className={`faultlog-pill status ${getStatusClass(fault.fault_status)}`}>
                    {toTitleCase(fault.fault_status)}
                  </span>
                </div>
              </div>

              <div className="faultlog-node-meta">
                {toTitleCase(fault.sensor_type)} · {formatFaultTimestamp(fault.ts)}
              </div>

              <div className="faultlog-node-description">
                <strong>Description:</strong> {fault.description || "No description provided."}
              </div>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}