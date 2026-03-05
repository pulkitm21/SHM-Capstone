import "./SensorStatus.css";

// This is a static component that displays the sensor status (online/offline) based on the isOnline prop. Will be linked to /status endpoint
export default function SensorStatus({ isOnline }: { isOnline: boolean }) {
  return (
    <div className="sensor-status">
      <div className="sensor-status-title">
        <span className="bold">Sensor Status:</span>{" "}
        <span className={isOnline ? "status-online" : "status-offline"}>
          {isOnline ? "Online" : "Offline"}
        </span>
      </div>
    </div>
  );
}
