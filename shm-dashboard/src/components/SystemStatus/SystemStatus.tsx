import "./SystemStatus.css";

// This is a static component that displays the system status (online/offline) based on the isOnline prop. Will be linked to /status endpoint
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
