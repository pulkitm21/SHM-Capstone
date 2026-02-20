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
    </div>
  );
}
