import { useEffect } from "react";
import "./ConfirmDialog.css";

export type ConfirmDialogTone = "neutral" | "warning" | "danger";

type ConfirmDialogProps = {
  open: boolean;
  title: string;
  message: string;
  confirmLabel?: string;
  cancelLabel?: string;
  confirmTone?: ConfirmDialogTone;
  onConfirm: () => void;
  onCancel: () => void;
};

function confirmButtonClass(tone: ConfirmDialogTone) {
  switch (tone) {
    case "danger":
      return "cd-btn cd-btn-danger";
    case "warning":
      return "cd-btn cd-btn-warning";
    default:
      return "cd-btn cd-btn-primary";
  }
}

export default function ConfirmDialog({
  open,
  title,
  message,
  confirmLabel = "Confirm",
  cancelLabel = "Cancel",
  confirmTone = "neutral",
  onConfirm,
  onCancel,
}: ConfirmDialogProps) {
  // Close the dialog when the user presses Escape.
  useEffect(() => {
    if (!open) return;

    function handleKeyDown(event: KeyboardEvent) {
      if (event.key === "Escape") {
        onCancel();
      }
    }

    window.addEventListener("keydown", handleKeyDown);
    return () => window.removeEventListener("keydown", handleKeyDown);
  }, [open, onCancel]);

  if (!open) return null;

  return (
    <div
      className="cd-backdrop"
      role="presentation"
      onClick={(event) => {
        if (event.target === event.currentTarget) {
          onCancel();
        }
      }}
    >
      <div
        className="cd-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="confirm-dialog-title"
      >
        <div className="cd-content">
          <h3 id="confirm-dialog-title" className="cd-title">
            {title}
          </h3>
          <p className="cd-message">{message}</p>
        </div>

        <div className="cd-actions">
          <button type="button" className="cd-btn cd-btn-secondary" onClick={onCancel}>
            {cancelLabel}
          </button>
          <button
            type="button"
            className={confirmButtonClass(confirmTone)}
            onClick={onConfirm}
          >
            {confirmLabel}
          </button>
        </div>
      </div>
    </div>
  );
}
