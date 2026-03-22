import { useEffect, useMemo, useState } from "react";
import "./SensorConfig.css";

export type AccelerometerOdrIndex = 0 | 1 | 2;
export type AccelerometerRange = 1 | 2 | 3;
export type ConfigSyncStatus = "unknown" | "synced" | "pending" | "failed";
export type ControlStatus = "idle" | "pending" | "acked" | "failed";
export type ControlCommand = "start" | "stop";
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
  pending_seq?: number | null;
  applied_seq?: number | null;
  last_ack_at?: string | null;
  sync_status: ConfigSyncStatus;

  pending_control_cmd?: ControlCommand | null;
  pending_control_seq?: number | null;
  last_control_cmd?: ControlCommand | null;
  last_control_seq?: number | null;
  last_control_ack_at?: string | null;
  control_status?: ControlStatus;
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
  pending_seq: null,
  applied_seq: null,
  last_ack_at: null,
  sync_status: "unknown",
  pending_control_cmd: null,
  pending_control_seq: null,
  last_control_cmd: null,
  last_control_seq: null,
  last_control_ack_at: null,
  control_status: "idle",
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

function prettySyncStatus(value: ConfigSyncStatus) {
  switch (value) {
    case "synced":
      return "Synced";
    case "pending":
      return "Pending";
    case "failed":
      return "Failed";
    default:
      return "Unknown";
  }
}

function prettyControlStatus(value: ControlStatus) {
  switch (value) {
    case "acked":
      return "Acked";
    case "pending":
      return "Pending";
    case "failed":
      return "Failed";
    default:
      return "Idle";
  }
}

function prettyState(value: NodeState) {
  switch (value) {
    case "idle":
      return "Idle";
    case "configured":
      return "Configured";
    case "recording":
      return "Recording";
    case "reconfig":
      return "Reconfiguring";
    case "error":
      return "Error";
    default:
      return "Unknown";
  }
}

function pillClassForSyncStatus(status: ConfigSyncStatus) {
  switch (status) {
    case "synced":
      return "sc-pill sc-pill-success";
    case "pending":
      return "sc-pill sc-pill-warning";
    case "failed":
      return "sc-pill sc-pill-danger";
    default:
      return "sc-pill";
  }
}

function pillClassForControlStatus(status: ControlStatus) {
  switch (status) {
    case "acked":
      return "sc-pill sc-pill-success";
    case "pending":
      return "sc-pill sc-pill-warning";
    case "failed":
      return "sc-pill sc-pill-danger";
    default:
      return "sc-pill";
  }
}

function pillClassForNodeState(state: NodeState) {
  switch (state) {
    case "recording":
      return "sc-pill sc-pill-success";
    case "configured":
    case "reconfig":
      return "sc-pill sc-pill-warning";
    case "error":
      return "sc-pill sc-pill-danger";
    default:
      return "sc-pill";
  }
}

function formatAckTime(value?: string | null, empty = "No ACK yet") {
  if (!value) return empty;

  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return "Invalid ACK time";

  return date.toLocaleString();
}

export default function SensorConfigCard({
  title = "Accelerometer Configuration",
  config,
  onApply,
  onStart,
  onStop,
  disabled = false,
}: {
  title?: string;
  config: SensorConfig;
  onApply: (updated: {
    odr_index: AccelerometerOdrIndex;
    range: AccelerometerRange;
    hpf_corner: number;
  }) => void;
  onStart: () => void;
  onStop: () => void;
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

  const displayConfig =
    safeConfig.sync_status === "synced"
      ? {
          odr: safeConfig.applied_odr_index ?? safeConfig.desired_odr_index,
          range: safeConfig.applied_range ?? safeConfig.desired_range,
          hpf: safeConfig.applied_hpf_corner ?? safeConfig.desired_hpf_corner,
        }
      : {
          odr: safeConfig.desired_odr_index,
          range: safeConfig.desired_range,
          hpf: safeConfig.desired_hpf_corner,
        };

  const startDisabled =
    disabled ||
    isEditing ||
    safeConfig.control_status === "pending" ||
    safeConfig.current_state === "recording";

  const stopDisabled =
    disabled ||
    isEditing ||
    safeConfig.control_status === "pending" ||
    safeConfig.current_state !== "recording";

  const applyDisabled =
    disabled || !isDirty || safeConfig.sync_status === "pending";

  function handleCancelEdit() {
    setDraftOdr(safeConfig.desired_odr_index);
    setDraftRange(safeConfig.desired_range);
    setDraftHpf(safeConfig.desired_hpf_corner);
    setIsEditing(false);
  }

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
          <p className="sc-subtitle">Live configuration and acquisition status for the selected node.</p>
        </div>

        <div className="sc-topbar-actions">
          <span className={pillClassForNodeState(safeConfig.current_state)}>
            {prettyState(safeConfig.current_state)}
          </span>

          <span className={pillClassForSyncStatus(safeConfig.sync_status)}>
            Config {prettySyncStatus(safeConfig.sync_status)}
          </span>

          <span className={pillClassForControlStatus(safeConfig.control_status ?? "idle")}>
            Control {prettyControlStatus(safeConfig.control_status ?? "idle")}
          </span>

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
          <span className="sc-item-label">ODR</span>
          {isEditing ? (
            <select
              className="sc-select"
              value={draftOdr}
              onChange={(e) => setDraftOdr(Number(e.target.value) as AccelerometerOdrIndex)}
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
              onChange={(e) => setDraftRange(Number(e.target.value) as AccelerometerRange)}
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
          <span className="sc-item-label">HPF Corner</span>
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

      <div className="sc-footer-row">
        <div className="sc-runtime-group">
          <div className="sc-runtime-chip">
            <span className="sc-runtime-label">Pending Config Seq</span>
            <span className="sc-runtime-value">{safeConfig.pending_seq ?? "—"}</span>
          </div>

          <div className="sc-runtime-chip">
            <span className="sc-runtime-label">Applied Config Seq</span>
            <span className="sc-runtime-value">{safeConfig.applied_seq ?? "—"}</span>
          </div>

          <div className="sc-runtime-chip">
            <span className="sc-runtime-label">Pending Control</span>
            <span className="sc-runtime-value">
              {safeConfig.pending_control_cmd
                ? `${safeConfig.pending_control_cmd} (${safeConfig.pending_control_seq ?? "—"})`
                : "—"}
            </span>
          </div>

          <div className="sc-runtime-chip">
            <span className="sc-runtime-label">Last Control</span>
            <span className="sc-runtime-value">
              {safeConfig.last_control_cmd
                ? `${safeConfig.last_control_cmd} (${safeConfig.last_control_seq ?? "—"})`
                : "—"}
            </span>
          </div>

          <div className="sc-runtime-chip sc-runtime-chip-wide">
            <span className="sc-runtime-label">Config ACK</span>
            <span className="sc-runtime-value sc-runtime-value-muted">
              {formatAckTime(safeConfig.last_ack_at)}
            </span>
          </div>

          <div className="sc-runtime-chip sc-runtime-chip-wide">
            <span className="sc-runtime-label">Control ACK</span>
            <span className="sc-runtime-value sc-runtime-value-muted">
              {formatAckTime(safeConfig.last_control_ack_at, "No control ACK yet")}
            </span>
          </div>
        </div>

        <div className="sc-action-group">
          {isEditing ? (
            <>
              <button type="button" className="sc-btn sc-btn-neutral" onClick={handleCancelEdit}>
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
            </>
          ) : (
            <>
              <button
                type="button"
                className="sc-btn sc-btn-neutral"
                onClick={onStart}
                disabled={startDisabled}
              >
                Start
              </button>
              <button
                type="button"
                className="sc-btn sc-btn-danger"
                onClick={onStop}
                disabled={stopDisabled}
              >
                Stop
              </button>
            </>
          )}
        </div>
      </div>

      {disabled && (
        <p className="sc-note">Configuration controls are only enabled for the accelerometer.</p>
      )}
    </section>
  );
}