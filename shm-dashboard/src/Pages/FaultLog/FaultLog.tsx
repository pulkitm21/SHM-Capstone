import FaultLog from "../../components/FaultLog/Log";
import "./FaultLog.css";

export default function FaultLogPage() {
  return (
    <div className="faultlog-page-shell">
      <div className="faultlog-page-hero">
        <div>
          <h1 className="faultlog-page-title">Fault Log</h1>
        </div>
      </div>

      <FaultLog variant="full" />
    </div>
  );
}