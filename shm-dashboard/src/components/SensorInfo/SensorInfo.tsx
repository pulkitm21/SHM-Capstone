import "./SensorInfo.css";

export type SensorMeta = {
  model: string;
  serial: string;
  installationDate: string;
  location: string;
  orientation: string;
};

export default function SensorInfoCard({ meta }: { meta: SensorMeta }) {
  return (
    <div className="sc-card">
      <div className="sc-card-title">Sensor Information</div>
      <div className="sc-card-body sc-two-col">
        <div className="sc-row">
          <span className="sc-muted">Model:</span>
          <span>{meta.model}</span>
        </div>
        <div className="sc-row">
          <span className="sc-muted">Serial number:</span>
          <span>{meta.serial}</span>
        </div>
        <div className="sc-row">
          <span className="sc-muted">Installation date:</span>
          <span>{meta.installationDate}</span>
        </div>
        <div className="sc-row">
          <span className="sc-muted">Location:</span>
          <span>{meta.location}</span>
        </div>
        <div className="sc-row">
          <span className="sc-muted">Orientation:</span>
          <span>{meta.orientation}</span>
        </div>
      </div>
    </div>
  );
}
