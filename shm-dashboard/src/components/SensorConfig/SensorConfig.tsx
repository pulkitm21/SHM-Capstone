import { useEffect, useState } from "react";
import "./SensorConfig.css";

export type SensorConfig = {
  samplingRate: "100" | "200" | "400";
  measurementRange: "2g" | "4g" | "8g";
  lowPassFilter: "none" | "50" | "100";
  highPassFilter: "none" | "1" | "5";
};

const DEFAULT_CONFIG: SensorConfig = {
  samplingRate: "400",
  measurementRange: "2g",
  lowPassFilter: "100",
  highPassFilter: "none",
};

function withDefaults(cfg: Partial<SensorConfig> | undefined | null): SensorConfig {
  return {
    samplingRate: cfg?.samplingRate ?? DEFAULT_CONFIG.samplingRate,
    measurementRange: cfg?.measurementRange ?? DEFAULT_CONFIG.measurementRange,
    lowPassFilter: cfg?.lowPassFilter ?? DEFAULT_CONFIG.lowPassFilter,
    highPassFilter: cfg?.highPassFilter ?? DEFAULT_CONFIG.highPassFilter,
  };
}

export default function SensorConfigCard({
  config,
  onSave,
}: {
  config: SensorConfig;
  onSave: (updated: SensorConfig) => void;
}) {
  const [isEditing, setIsEditing] = useState(false);
  const [draft, setDraft] = useState<SensorConfig>(() => withDefaults(config));

  useEffect(() => {
    setDraft(withDefaults(config));
    setIsEditing(false);
  }, [config]);

  function handleCancel() {
    setDraft(withDefaults(config));
    setIsEditing(false);
  }

  function handleSave() {
    onSave(withDefaults(draft));
    setIsEditing(false);
  }

  const safeConfig = withDefaults(config);

  return (
    <div className="sc-card">
      <div className="sc-card-title sc-card-title-row">
        <span>Sensor Configuration</span>

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

      <div className="sc-card-body sc-config-grid">
        <div className="sc-config-row">
          <span className="sc-muted">Sampling rate:</span>

          {isEditing ? (
            <div className="sc-pill-group">
              {(["100", "200", "400"] as const).map((v) => (
                <button
                  key={v}
                  type="button"
                  className={`sc-pill ${draft.samplingRate === v ? "active" : ""}`}
                  onClick={() => setDraft((d) => ({ ...d, samplingRate: v }))}
                >
                  {v} Hz
                </button>
              ))}
            </div>
          ) : (
            <span className="sc-pill">{safeConfig.samplingRate} Hz</span>
          )}
        </div>

        <div className="sc-config-row">
          <span className="sc-muted">Measurement range:</span>

          {isEditing ? (
            <div className="sc-pill-group">
              {(["2g", "4g", "8g"] as const).map((v) => (
                <button
                  key={v}
                  type="button"
                  className={`sc-pill ${draft.measurementRange === v ? "active" : ""}`}
                  onClick={() => setDraft((d) => ({ ...d, measurementRange: v }))}
                >
                  ±{v.replace("g", "")} g
                </button>
              ))}
            </div>
          ) : (
            <span className="sc-pill">±{safeConfig.measurementRange.replace("g", "")} g</span>
          )}
        </div>

        <div className="sc-config-row">
          <span className="sc-muted">Low-pass filter:</span>

          {isEditing ? (
            <div className="sc-pill-group">
              {(["none", "50", "100"] as const).map((v) => (
                <button
                  key={v}
                  type="button"
                  className={`sc-pill ${draft.lowPassFilter === v ? "active" : ""}`}
                  onClick={() => setDraft((d) => ({ ...d, lowPassFilter: v }))}
                >
                  {v === "none" ? "None" : `${v} Hz`}
                </button>
              ))}
            </div>
          ) : (
            <span className="sc-pill">
              {safeConfig.lowPassFilter === "none" ? "None" : `${safeConfig.lowPassFilter} Hz`}
            </span>
          )}
        </div>

        <div className="sc-config-row">
          <span className="sc-muted">High-pass filter:</span>

          {isEditing ? (
            <div className="sc-pill-group">
              {(["none", "1", "5"] as const).map((v) => (
                <button
                  key={v}
                  type="button"
                  className={`sc-pill ${draft.highPassFilter === v ? "active" : ""}`}
                  onClick={() => setDraft((d) => ({ ...d, highPassFilter: v }))}
                >
                  {v === "none" ? "None" : `${v} Hz`}
                </button>
              ))}
            </div>
          ) : (
            <span className="sc-pill">
              {safeConfig.highPassFilter === "none" ? "None" : `${safeConfig.highPassFilter} Hz`}
            </span>
          )}
        </div>

        {/* RMS warning threshold removed */}
      </div>
    </div>
  );
}
