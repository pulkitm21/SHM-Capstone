import "./SensorConfig.css";

export default function SensorConfig() {
  return (
    <div className="sc-card">
      <div className="sc-card-title">Sensor Configuration</div>

      <div className="sc-card-body sc-config-grid">
        <div className="sc-config-row">
          <span className="sc-muted">Sampling rate:</span>
          <select className="sc-mini-select" defaultValue="400">
            <option value="100">100 Hz</option>
            <option value="200">200 Hz</option>
            <option value="400">400 Hz</option>
          </select>
        </div>

        <div className="sc-config-row">
          <span className="sc-muted">Measurement range:</span>
          <select className="sc-mini-select" defaultValue="2g">
            <option value="2g">±2 g</option>
            <option value="4g">±4 g</option>
            <option value="8g">±8 g</option>
          </select>
        </div>

        <div className="sc-config-row">
          <span className="sc-muted">Low-pass filter:</span>
          <select className="sc-mini-select" defaultValue="100">
            <option value="none">None</option>
            <option value="50">50 Hz</option>
            <option value="100">100 Hz</option>
          </select>
        </div>

        <div className="sc-config-row">
          <span className="sc-muted">High-pass filter:</span>
          <select className="sc-mini-select" defaultValue="none">
            <option value="none">None</option>
            <option value="1">1 Hz</option>
            <option value="5">5 Hz</option>
          </select>
        </div>

        <div className="sc-config-row">
          <span className="sc-muted">RMS warning threshold:</span>
          <span className="sc-pill">0.2 g</span>
        </div>
      </div>
    </div>
  );
}
