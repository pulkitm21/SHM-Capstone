import { useEffect, useMemo, useState } from "react";
import "./SensorConfig.css";

export type AccelerometerOdrIndex = 0 | 1 | 2;
export type AccelerometerRange = 1 | 2 | 3;
export type ControlCommand = "start" | "stop" | "reset";
export type NodeState = "unknown" | "idle" | "configured" | "recording" | "reconfig" | "error";

export type SensorConfig = {
  odr_index: AccelerometerOdrIndex;
  range: AccelerometerRange;
  hpf_corner: number;

  desired_odr_index: AccelerometerOdrIndex;
  desired_range: AccelerometerRange;
  desired_hpf_corner: number;

  applied_odr_index: AccelerometerOdrIndex | null;
  applied_range: AccelerometerRange | null;
  applied_hpf_corner: number | null;

  current_state: NodeState;

  // These are kept optional for compatibility with any cached/settings payloads.
  pending_seq?: number | null;
  applied_seq?: number | null;
  last_ack_at?: string | null;
  sync_status?: string | null;

  pending_control_cmd?: ControlCommand | null;
  pending_control_seq?: number | null;
  last_control_cmd?: ControlCommand | null;
  last_control_seq?: number | null;
  last_control_ack_at?: string | null;
  control_status?: string | null;
};

const DEFAULT_CONFIG: SensorConfig = {
  odr_index: 2,
  range: 1,
  hpf_corner: 0,
  desired_odr_index: 2,
  desired_range: 1,
  desired_hpf_corner: 0,
  applied_odr_index: null,
  applied_range: null,
  applied_hpf_corner: null,
  current_state: "unknown",

    // ACK/SEQ fields retained only as inert compatibility fields.
  pending_seq: null,
  applied_seq: null,
  last_ack_at: null,
  sync_status: null,
  pending_control_cmd: null,
  pending_control_seq: null,
  last_control_cmd: null,
  last_control_seq: null,
  last_control_ack_at: null,
  control_status: null,
};

function withDefaults(cfg?: Partial<SensorConfig> | null): SensorConfig {
  return {
    odr_index: cfg?.odr_index ?? DEFAULT_CONFIG.odr_index,
    range: cfg?.range ?? DEFAULT_CONFIG.range,
    hpf_corner: cfg?.hpf_corner ?? DEFAULT_CONFIG.hpf_corner,
    desired_odr_index:
      cfg?.desired_odr_index ?? cfg?.odr_index ?? DEFAULT_CONFIG.desired_odr_index,
    desired_range: cfg?.desired_range ?? cfg?.range ?? DEFAULT_CONFIG.desired_range,
    desired_hpf_corner:
      cfg?.desired_hpf_corner ?? cfg?.hpf_corner ?? DEFAULT_CONFIG.desired_hpf_corner,
    applied_odr_index: cfg?.applied_odr_index ?? DEFAULT_CONFIG.applied_odr_index,
    applied_range: cfg?.applied_range ?? DEFAULT_CONFIG.applied_range,
    applied_hpf_corner: cfg?.applied_hpf_corner ?? DEFAULT_CONFIG.applied_hpf_corner,
    current_state: cfg?.current_state ?? DEFAULT_CONFIG.current_state,
    pending_seq: cfg?.pending_seq ?? DEFAULT_CONFIG.pending_seq,
    applied_seq: cfg?.applied_seq ?? DEFAULT_CONFIG.applied_seq,
    last_ack_at: cfg?.last_ack_at ?? DEFAULT_CONFIG.last_ack_at,
    sync_status: cfg?.sync_status ?? DEFAULT_CONFIG.sync_status,
    pending_control_cmd: cfg?.pending_control_cmd ?? DEFAULT_CONFIG.pending_control_cmd,
    pending_control_seq: cfg?.pending_control_seq ?? DEFAULT_CONFIG.pending_control_seq,
    last_control_cmd: cfg?.last_control_cmd ?? DEFAULT_CONFIG.last_control_cmd,
    last_control_seq: cfg?.last_control_seq ?? DEFAULT_CONFIG.last_control_seq,
    last_control_ack_at: cfg?.last_control_ack_at ?? DEFAULT_CONFIG.last_control_ack_at,
    control_status: cfg?.control_status ?? DEFAULT_CONFIG.control_status,
  };
}

function prettyOdr(index: AccelerometerOdrIndex | null) {
  if (index === null) return "—";
  if (index === 0) return "4000 Hz";
  if (index === 1) return "2000 Hz";
  return "1000 Hz";
}

function prettyDraftOdr(index: AccelerometerOdrIndex) {
  if (index === 0) return "0 (4000 Hz)";
  if (index === 1) return "1 (2000 Hz)";
  return "2 (1000 Hz)";
}

function prettyRange(value: AccelerometerRange | null) {
  if (value === null) return "—";
  if (value === 1) return "±2g";
  if (value === 2) return "±4g";
  return "±8g";
}

export default function SensorConfigCard({
  title = "Accelerometer Configuration",
  config,
  onApply,
  disabled = false,
}: {
  title?: string;
  config: SensorConfig;
  onApply: (updated: {
    odr_index: AccelerometerOdrIndex;
    range: AccelerometerRange;
    hpf_corner: number;
  }) => void;
  disabled?: boolean;
}) {
  const safeConfig = useMemo(() => withDefaults(config), [config]);

  const [draftOdr, setDraftOdr] = useState<AccelerometerOdrIndex>(
    safeConfig.desired_odr_index
  );
  const [draftRange, setDraftRange] = useState<AccelerometerRange>(
    safeConfig.desired_range
  );
  const [draftHpf, setDraftHpf] = useState<number>(safeConfig.desired_hpf_corner);
  const [isEditing, setIsEditing] = useState(false);

  // Keep the local draft aligned with backend-backed config changes.
  useEffect(() => {
    setDraftOdr(safeConfig.desired_odr_index);
    setDraftRange(safeConfig.desired_range);
    setDraftHpf(safeConfig.desired_hpf_corner);
  }, [
    safeConfig.desired_odr_index,
    safeConfig.desired_range,
    safeConfig.desired_hpf_corner,
  ]);

  const isDirty =
    draftOdr !== safeConfig.desired_odr_index ||
    draftRange !== safeConfig.desired_range ||
    draftHpf !== safeConfig.desired_hpf_corner;

  // Always show desired config rather than ACK-dependent values.
  const displayConfig = {
    odr: safeConfig.desired_odr_index,
    range: safeConfig.desired_range,
    hpf: safeConfig.desired_hpf_corner,
  };

  const applyDisabled = disabled || !isDirty;

  // Reset the local edit state without touching backend state.
  function handleCancelEdit() {
    setDraftOdr(safeConfig.desired_odr_index);
    setDraftRange(safeConfig.desired_range);
    setDraftHpf(safeConfig.desired_hpf_corner);
    setIsEditing(false);
  }

  // Send the updated accelerometer config to the page handler.
  function handleApplyClick() {
    onApply({
      odr_index: draftOdr,
      range: draftRange,
      hpf_corner: draftHpf,
    });
    setIsEditing(false);
  }

  return (
    <section className="sc-shell">
      <div className="sc-topbar">
        <div>
          <h2 className="sc-title">{title}</h2>
          <p className="sc-subtitle">
            Configuration values for the selected node.
          </p>
        </div>

        <div className="sc-topbar-actions">
          {!disabled && !isEditing && (
            <button
              type="button"
              className="sc-btn sc-btn-neutral"
              onClick={() => setIsEditing(true)}
            >
              Edit
            </button>
          )}
        </div>
      </div>

      <div className="sc-grid">
        <div className="sc-item">
          <div className="sc-label-row">
            <span className="sc-item-label">ODR</span>

            {/* Tooltip for the ODR field. */}
            <span className="sc-tooltip-wrap">
              <button
                type="button"
                className="sc-tooltip-trigger"
                aria-label="More information about ODR"
              >
                ?
              </button>
              <span className="sc-tooltip-bubble">
                Output Data Rate. This controls how often the accelerometer
                samples data. Higher values capture faster vibration changes, 
                and are more resistance to noise, currently all ODR's downsample to 200Hz.
              </span>
            </span>
          </div>

          {isEditing ? (
            <select
              className="sc-select"
              value={draftOdr}
              onChange={(e) =>
                setDraftOdr(Number(e.target.value) as AccelerometerOdrIndex)
              }
              disabled={disabled}
            >
              <option value={0}>{prettyDraftOdr(0)}</option>
              <option value={1}>{prettyDraftOdr(1)}</option>
              <option value={2}>{prettyDraftOdr(2)}</option>
            </select>
          ) : (
            <span className="sc-value-pill">{prettyOdr(displayConfig.odr)}</span>
          )}
        </div>

        <div className="sc-item">
          <span className="sc-item-label">Range</span>
          {isEditing ? (
            <select
              className="sc-select"
              value={draftRange}
              onChange={(e) =>
                setDraftRange(Number(e.target.value) as AccelerometerRange)
              }
              disabled={disabled}
            >
              <option value={1}>±2g</option>
              <option value={2}>±4g</option>
              <option value={3}>±8g</option>
            </select>
          ) : (
            <span className="sc-value-pill">{prettyRange(displayConfig.range)}</span>
          )}
        </div>

        <div className="sc-item">
          <div className="sc-label-row">
            <span className="sc-item-label">HPF Corner</span>

            {/* Tooltip for the HPF field. */}
            <span className="sc-tooltip-wrap">
              <button
                type="button"
                className="sc-tooltip-trigger"
                aria-label="More information about HPF Corner"
              >
                ?
              </button>
              <span className="sc-tooltip-bubble">
                High-pass filter corner setting. This reduces low-frequency
                content in the signal. A value of 0 keeps the filter off.
              </span>
            </span>
          </div>

          {isEditing ? (
            <select
              className="sc-select"
              value={draftHpf}
              onChange={(e) => setDraftHpf(Number(e.target.value))}
              disabled={disabled}
            >
              {[0, 1, 2, 3, 4, 5, 6].map((value) => (
                <option key={value} value={value}>
                  {value}
                </option>
              ))}
            </select>
          ) : (
            <span className="sc-value-pill">{displayConfig.hpf}</span>
          )}
        </div>
      </div>

      {isEditing && (
        <div className="sc-footer-row">
          <div className="sc-action-group">
            <button
              type="button"
              className="sc-btn sc-btn-neutral"
              onClick={handleCancelEdit}
            >
              Cancel
            </button>
            <button
              type="button"
              className="sc-btn sc-btn-success"
              onClick={handleApplyClick}
              disabled={applyDisabled}
            >
              Apply
            </button>
          </div>
        </div>
      )}

      {disabled && (
        <p className="sc-note">
          Configuration editing is only enabled for the accelerometer.
        </p>
      )}
    </section>
  );
}