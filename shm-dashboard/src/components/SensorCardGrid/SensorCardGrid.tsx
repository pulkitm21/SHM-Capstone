import type { SensorValue } from "../../Pages/SensorManagement/SensorManagement.preview";
import "./SensorCardGrid.css";

export type SensorCardItem = {
  label: string;
  value: SensorValue;
  model: string;
  status: "online" | "offline" | "warning" | "idle";
  lastDataAgeText: string;
  activeFaultCount: number;
  latestFaultText: string | null;
};

type Props = {
  sensors: SensorCardItem[];
  selectedSensor: SensorValue;
  onSelectSensor: (sensor: SensorValue) => void;
};

export default function SensorCardGrid({
  sensors,
  selectedSensor,
  onSelectSensor,
}: Props) {
  return (
    <div className="sm-panel">
      <div className="sm-panel-header">
        <h2 className="sm-panel-title">Sensors</h2>
      </div>

      <div className="sm-sensor-grid">
        {sensors.map((item) => {
          const selected = item.value === selectedSensor;

          return (
            <button
              key={item.value}
              type="button"
              className={`sm-sensor-card ${selected ? "selected" : ""}`}
              onClick={() => onSelectSensor(item.value)}
            >
              <div className="sm-sensor-card-top">
                <div>
                  <div className="sm-sensor-title">{item.label}</div>
                  <div className="sm-sensor-subtitle">{item.model || "—"}</div>
                </div>

                <span className={`sm-health-pill ${item.status}`}>
                  {item.status === "online"
                    ? "Online"
                    : item.status === "offline"
                      ? "Offline"
                      : item.status === "idle"
                        ? "Idle"
                        : "Warning"}
                </span>
              </div>

              {/* Show a quick operational summary for each sensor card. */}
              <div className="sm-sensor-meta">
                <div className="sm-sensor-meta-row">
                  <span className="sm-sensor-meta-label">Last data</span>
                  <span className="sm-sensor-meta-value">{item.lastDataAgeText}</span>
                </div>

                <div className="sm-sensor-meta-row">
                  <span className="sm-sensor-meta-label">Active faults</span>
                  <span className="sm-sensor-meta-value">{item.activeFaultCount}</span>
                </div>

                <div className="sm-sensor-meta-row">
                  <span className="sm-sensor-meta-label">Latest fault</span>
                  <span className="sm-sensor-meta-value">
                    {item.latestFaultText || "—"}
                  </span>
                </div>
              </div>
            </button>
          );
        })}
      </div>
    </div>
  );
}