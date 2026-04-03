import type { ReactNode } from "react";
import "./InfoTooltip.css";

export default function InfoTooltip({
  label = "More information",
  content,
}: {
  label?: string;
  content: ReactNode;
}) {
  return (
    <span className="it-shell">
      {/* Small trigger button that shows the tooltip on hover or focus. */}
      <button
        type="button"
        className="it-trigger"
        aria-label={label}
      >
        ?
      </button>

      {/* Tooltip bubble with explanatory helper text. */}
      <span role="tooltip" className="it-bubble">
        {content}
      </span>
    </span>
  );
}