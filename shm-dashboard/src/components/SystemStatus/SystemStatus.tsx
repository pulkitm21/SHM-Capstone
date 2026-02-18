import "./SystemStatus.css";

export default function SystemStatus({ isOnline }: { isOnline: boolean }) {
  return (
    <div className="system-status">
      <div className="system-status-title">
        <span className="bold">System Status:</span>{" "}
        <span className={isOnline ? "status-online" : "status-offline"}>
          {isOnline ? "Online" : "Offline"}
        </span>
      </div>

      <div className="system-status-legend">
        <div className="legend-item">
          <span className="dot dot-online" />
          <span>Online</span>
        </div>
        <div className="legend-item">
          <span className="dot dot-offline" />
          <span>Offline</span>
        </div>
      </div>
    </div>
  );
}
