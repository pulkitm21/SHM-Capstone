import { useEffect, useState } from "react";
import "./SensorInfo.css";

export type SensorMeta = {
  model: string;
  serial: string;
  installationDate: string;
  location: string;
  orientation: string;
};

export default function SensorInfoCard({
  meta,
  onSave,
}: {
  meta: SensorMeta;
  onSave: (updated: SensorMeta) => void;
}) {
  const [isEditing, setIsEditing] = useState(false);
  const [draft, setDraft] = useState<SensorMeta>(meta);

  useEffect(() => {
    setDraft(meta);
    setIsEditing(false);
  }, [meta]);

  function handleCancel() {
    setDraft(meta);
    setIsEditing(false);
  }

  function handleSave() {
    onSave(draft);
    setIsEditing(false);
  }

  return (
    <div className="sc-card">
      <div className="sc-card-title sc-card-title-row">
        <span>Sensor Information</span>

        {!isEditing ? (
          <button className="sc-btn sc-btn-ghost" onClick={() => setIsEditing(true)}>
            Edit
          </button>
        ) : (
          <div className="sc-btn-row">
            <button className="sc-btn sc-btn-danger" onClick={handleCancel}>
              Cancel
            </button>
            <button className="sc-btn sc-btn-success" onClick={handleSave}>
              Save
            </button>
          </div>
        )}
      </div>

      <div className="sc-card-body sc-two-col">
        <div className="sc-row">
          <span className="sc-muted">Model:</span>
          {isEditing ? (
            <input className="sc-input" value={draft.model} onChange={(e) => setDraft((d) => ({ ...d, model: e.target.value }))} />
          ) : (
            <span>{meta.model}</span>
          )}
        </div>

        <div className="sc-row">
          <span className="sc-muted">Serial number:</span>
          {isEditing ? (
            <input className="sc-input" value={draft.serial} onChange={(e) => setDraft((d) => ({ ...d, serial: e.target.value }))} />
          ) : (
            <span>{meta.serial}</span>
          )}
        </div>

        <div className="sc-row">
          <span className="sc-muted">Installation date:</span>
          {isEditing ? (
            <input
              className="sc-input"
              type="date"
              value={draft.installationDate || ""}
              onChange={(e) => setDraft((d) => ({ ...d, installationDate: e.target.value }))}
            />
          ) : (
            <span>{meta.installationDate}</span>
          )}
        </div>

        <div className="sc-row">
          <span className="sc-muted">Location:</span>
          {isEditing ? (
            <input className="sc-input" value={draft.location} onChange={(e) => setDraft((d) => ({ ...d, location: e.target.value }))} />
          ) : (
            <span>{meta.location}</span>
          )}
        </div>

        <div className="sc-row">
          <span className="sc-muted">Orientation:</span>
          {isEditing ? (
            <input className="sc-input" value={draft.orientation} onChange={(e) => setDraft((d) => ({ ...d, orientation: e.target.value }))} />
          ) : (
            <span>{meta.orientation}</span>
          )}
        </div>
      </div>
    </div>
  );
}
