import { useEffect, useState } from "react";
import "./SensorConfig.css";

export type HpfValue = "none" | "on";
export type ConfigSyncStatus = "unknown" | "synced" | "pending" | "failed";

export type SensorConfig = {
  highPassFilterDesired: HpfValue;
  highPassFilterApplied: HpfValue | null;
  highPassFilterStatus: ConfigSyncStatus;
  lastRequestId?: string;
  lastAckAt?: string | null;
};

const DEFAULT_CONFIG: SensorConfig = {
  highPassFilterDesired: "none",
  highPassFilterApplied: null,
  highPassFilterStatus: "unknown",
  lastRequestId: undefined,
  lastAckAt: null,
};

function withDefaults(cfg: Partial<SensorConfig> | undefined | null): SensorConfig {
  return {
    highPassFilterDesired: cfg?.highPassFilterDesired ?? DEFAULT_CONFIG.highPassFilterDesired,
    highPassFilterApplied: cfg?.highPassFilterApplied ?? DEFAULT_CONFIG.highPassFilterApplied,
    highPassFilterStatus: cfg?.highPassFilterStatus ?? DEFAULT_CONFIG.highPassFilterStatus,
    lastRequestId: cfg?.lastRequestId ?? DEFAULT_CONFIG.lastRequestId,
    lastAckAt: cfg?.lastAckAt ?? DEFAULT_CONFIG.lastAckAt,
  };
}

function prettyHpf(value: HpfValue | null) {
  if (value === null) return "Unknown";
  return value === "none" ? "Off" : "On";
}

function prettyStatus(value: ConfigSyncStatus) {
  switch (value) {
    case "synced":
      return "Applied";
    case "pending":
      return "Pending";
    case "failed":
      return "Failed";
    default:
      return "Unknown";
  }
}

export default function SensorConfigCard({
  config,
  onSave,
  disabled = false,
}: {
  config: SensorConfig;
  onSave: (updatedDesired: HpfValue) => void;
  disabled?: boolean;
}) {
  const safeConfig = withDefaults(config);
  const [isEditing, setIsEditing] = useState(false);
  const [draft, setDraft] = useState<HpfValue>(safeConfig.highPassFilterDesired);

  useEffect(() => {
    setDraft(safeConfig.highPassFilterDesired);
    setIsEditing(false);
  }, [safeConfig.highPassFilterDesired]);

  function handleCancel() {
    setDraft(safeConfig.highPassFilterDesired);
    setIsEditing(false);
  }

  function handleSave() {
    onSave(draft);
    setIsEditing(false);
  }

  return (
    <div className="sc-card">
      <div className="sc-card-title sc-card-title-row">
        <span>Accelerometer Configuration</span>

        {!isEditing ? (
          <button
            className="sc-btn sc-btn-ghost"
            onClick={() => setIsEditing(true)}
            disabled={disabled}
          >
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

      <div className="sc-card-body sc-config-grid">
        <div className="sc-config-row">
          <span className="sc-muted">Desired HPF:</span>

          {isEditing ? (
            <div className="sc-pill-group">
              {(["none", "on"] as const).map((v) => (
                <button
                  key={v}
                  type="button"
                  className={`sc-pill ${draft === v ? "active" : ""}`}
                  onClick={() => setDraft(v)}
                >
                  {v === "none" ? "Off" : "On"}
                </button>
              ))}
            </div>
          ) : (
            <span className="sc-pill">{prettyHpf(safeConfig.highPassFilterDesired)}</span>
          )}
        </div>

        <div className="sc-config-row">
          <span className="sc-muted">Applied HPF:</span>
          <span className="sc-pill">{prettyHpf(safeConfig.highPassFilterApplied)}</span>
        </div>

        <div className="sc-config-row">
          <span className="sc-muted">Sync status:</span>
          <span className="sc-pill">{prettyStatus(safeConfig.highPassFilterStatus)}</span>
        </div>

        <div className="sc-config-row">
          <span className="sc-muted">Last ACK:</span>
          <span className="sc-pill">
            {safeConfig.lastAckAt ? new Date(safeConfig.lastAckAt).toLocaleString() : "—"}
          </span>
        </div>
      </div>
    </div>
  );
}