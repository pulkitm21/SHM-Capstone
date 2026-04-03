import { useState } from "react";
import ConfirmDialog from "../ConfirmDialog/ConfirmDialog";
import type { NodeState } from "../SensorConfig/SensorConfig";
import "./NodeControlCard.css";

type NodeControlCardProps = {
  title?: string;
  state: NodeState;
  disabled?: boolean;
  onStart: () => void;
  onStop: () => void;
  onReset: () => void;
};

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

function pillClassForNodeState(state: NodeState) {
  switch (state) {
    case "recording":
      return "nc-pill nc-pill-success";
    case "configured":
    case "reconfig":
      return "nc-pill nc-pill-warning";
    case "error":
      return "nc-pill nc-pill-danger";
    default:
      return "nc-pill";
  }
}

export default function NodeControlCard({
  title = "Node Control",
  state,
  disabled = false,
  onStart,
  onStop,
  onReset,
}: NodeControlCardProps) {
  const [isResetDialogOpen, setIsResetDialogOpen] = useState(false);

  const startDisabled = disabled || state === "recording";
  const stopDisabled = disabled || state !== "recording";
  const resetDisabled = disabled;

  // Show a confirmation dialog before sending reset.
  function handleConfirmReset() {
    setIsResetDialogOpen(false);
    onReset();
  }

  return (
    <>
      <section className="nc-shell">
        <div className="nc-topbar">
          <div>
            <h2 className="nc-title">{title}</h2>
            <p className="nc-subtitle">
              Start, stop, and reset the selected node.
            </p>
          </div>

          <div className="nc-topbar-actions">
            <span className={pillClassForNodeState(state)}>{prettyState(state)}</span>
          </div>
        </div>

        <div className="nc-action-group">
          <button
            type="button"
            className="nc-btn nc-btn-neutral"
            onClick={onStart}
            disabled={startDisabled}
          >
            Start
          </button>
          <button
            type="button"
            className="nc-btn nc-btn-danger"
            onClick={onStop}
            disabled={stopDisabled}
          >
            Stop
          </button>
          <button
            type="button"
            className="nc-btn nc-btn-warning"
            onClick={() => setIsResetDialogOpen(true)}
            disabled={resetDisabled}
          >
            Reset
          </button>
        </div>

        {disabled && (
          <p className="nc-note">Runtime controls are only available to admin users.</p>
        )}
      </section>

      <ConfirmDialog
        open={isResetDialogOpen}
        title="Reset selected node?"
        message="This will send a reset command to the selected node. Continue only if you intend to restart its runtime state."
        confirmLabel="Yes, reset node"
        cancelLabel="Cancel"
        confirmTone="warning"
        onConfirm={handleConfirmReset}
        onCancel={() => setIsResetDialogOpen(false)}
      />
    </>
  );
}
