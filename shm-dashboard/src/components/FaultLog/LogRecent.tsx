import type { FaultRow } from "../../services/api";
import {
  formatFaultTimestamp,
  formatRelativeTime,
  getSeverityClass,
  toTitleCase,
} from "./faultLog.utils";

type Props = {
  faults: FaultRow[];
  loading: boolean;
  error: string;
};

export default function LogRecent({ faults, loading, error }: Props) {
  return (
    <div className="faultlog-panel">
      {loading ? (
        <div className="faultlog-empty">Loading faults…</div>
      ) : error ? (
        <div className="faultlog-empty">Unable to load faults: {error}</div>
      ) : faults.length === 0 ? (
        <div className="faultlog-empty">No recent faults.</div>
      ) : (
        <div className="faultlog-recent-list">
          {faults.map((fault) => (
            <div key={fault.id} className="faultlog-recent-item">
              <div className="faultlog-recent-main">
                <div className="faultlog-recent-title">{fault.fault_type || "Unknown fault"}</div>
                <div className="faultlog-recent-meta">
                  {toTitleCase(fault.sensor_type)} · {fault.serial_number || "Backend"} ·{" "}
                  {formatFaultTimestamp(fault.ts)}
                </div>
              </div>

              <div className="faultlog-recent-side">
                <span className={`faultlog-pill severity ${getSeverityClass(Number(fault.severity))}`}>
                  Severity {fault.severity ?? "—"}
                </span>
                <div className="faultlog-recent-relative">{formatRelativeTime(fault.ts)}</div>
              </div>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}