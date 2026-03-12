import FaultLog from "../../components/FaultLog/Log";
import "./FaultLog.css";

export default function FaultLogPage() {
  return (
    <div className="faultlog-page">
      <h2 className="faultlog-title">Fault Log</h2>
      <div className="faultlog-card">
        <FaultLog limit={500} />
      </div>
    </div>
  );
}