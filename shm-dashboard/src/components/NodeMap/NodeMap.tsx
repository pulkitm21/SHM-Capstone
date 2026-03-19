import { useEffect, useMemo, useRef, useState } from "react";
import {
  putNodePositions,
  type NodeRecord,
} from "../../services/api";
import "./NodeMap.css";

type NodePos = { x: number; y: number };
type NodePosMap = Record<string, NodePos>;

type NodeMapProps = {
  nodes: NodeRecord[];
  warningSerials?: string[];
  onNodeClick?: (node: NodeRecord) => void;
};

const MIN_X = 0.06;
const MAX_X = 0.94;
const MIN_Y = 0.08;
const MAX_Y = 0.94;

const OVERLAP_X_GAP = 0.05;
const OVERLAP_Y_GAP = 0.05;

function clamp(n: number, min: number, max: number) {
  return Math.max(min, Math.min(max, n));
}

function buildPositions(nodes: NodeRecord[]): NodePosMap {
  const out: NodePosMap = {};

  nodes.forEach((node, idx) => {
    const hasBackendPosition =
      typeof node.x === "number" && typeof node.y === "number";

    if (hasBackendPosition) {
      out[node.serial] = {
        x: clamp(node.x as number, MIN_X, MAX_X),
        y: clamp(node.y as number, MIN_Y, MAX_Y),
      };
      return;
    }

    const y = 0.20 + idx * (0.65 / Math.max(1, nodes.length - 1));
    out[node.serial] = { x: 0.50, y: clamp(y, 0.12, 0.90) };
  });

  return out;
}

function buildResetPositions(nodes: NodeRecord[]): NodePosMap {
  const out: NodePosMap = {};
  const startX = 0.12;
  const startY = 0.88;
  const stepX = 0.065;
  const stepY = 0.07;
  const perRow = 5;

  nodes.forEach((node, idx) => {
    const col = idx % perRow;
    const row = Math.floor(idx / perRow);

    out[node.serial] = {
      x: clamp(startX + col * stepX, MIN_X, MAX_X),
      y: clamp(startY - row * stepY, MIN_Y, MAX_Y),
    };
  });

  return out;
}

function isPositionAvailable(
  serial: string,
  nextPos: NodePos,
  positions: NodePosMap
) {
  return Object.entries(positions).every(([otherSerial, otherPos]) => {
    if (otherSerial === serial) return true;

    const dx = Math.abs(nextPos.x - otherPos.x);
    const dy = Math.abs(nextPos.y - otherPos.y);

    return !(dx < OVERLAP_X_GAP && dy < OVERLAP_Y_GAP);
  });
}

export default function NodeMap({
  nodes,
  warningSerials = [],
  onNodeClick,
}: NodeMapProps) {
  const sortedNodes = useMemo(
    () => [...nodes].sort((a, b) => a.node_id - b.node_id),
    [nodes]
  );

  const warningSet = useMemo(() => new Set(warningSerials), [warningSerials]);

  const containerRef = useRef<HTMLDivElement | null>(null);
  const pointerStartRef = useRef<{ x: number; y: number } | null>(null);
  const movedRef = useRef(false);

  const [isEditing, setIsEditing] = useState(false);
  const [savedPositions, setSavedPositions] = useState<NodePosMap>(() =>
    buildPositions(sortedNodes)
  );
  const [draftPositions, setDraftPositions] = useState<NodePosMap>(() =>
    buildPositions(sortedNodes)
  );
  const [draggingSerial, setDraggingSerial] = useState<string | null>(null);
  const [isSaving, setIsSaving] = useState(false);
  const [saveError, setSaveError] = useState("");

  useEffect(() => {
    const nextPositions = buildPositions(sortedNodes);
    setSavedPositions(nextPositions);

    if (!isEditing) {
      setDraftPositions(nextPositions);
    }
  }, [sortedNodes, isEditing]);

  function beginEditMode() {
    setSaveError("");
    setDraftPositions(savedPositions);
    setIsEditing(true);
  }

  function cancelEditMode() {
    setSaveError("");
    setDraggingSerial(null);
    pointerStartRef.current = null;
    movedRef.current = false;
    setDraftPositions(savedPositions);
    setIsEditing(false);
  }

  async function handleSave() {
    try {
      setIsSaving(true);
      setSaveError("");

      const payload = {
        positions: sortedNodes
          .map((node) => {
            const pos = draftPositions[node.serial];
            if (!pos) return null;

            return {
              node_id: node.node_id,
              x: clamp(pos.x, MIN_X, MAX_X),
              y: clamp(pos.y, MIN_Y, MAX_Y),
            };
          })
          .filter(Boolean) as { node_id: number; x: number; y: number }[],
      };

      await putNodePositions(payload);

      setSavedPositions(draftPositions);
      setIsEditing(false);
    } catch (error) {
      console.error("Failed to save node positions:", error);
      setSaveError("Failed to save node positions.");
    } finally {
      setIsSaving(false);
    }
  }

  function handleReset() {
    setDraftPositions(buildResetPositions(sortedNodes));
  }

  function setNodePositionFromClientPoint(
    serial: string,
    clientX: number,
    clientY: number
  ) {
    const el = containerRef.current;
    if (!el) return;

    const rect = el.getBoundingClientRect();
    const nextPos = {
      x: clamp((clientX - rect.left) / rect.width, MIN_X, MAX_X),
      y: clamp((clientY - rect.top) / rect.height, MIN_Y, MAX_Y),
    };

    setDraftPositions((prev) => {
      if (!isPositionAvailable(serial, nextPos, prev)) {
        return prev;
      }

      return { ...prev, [serial]: nextPos };
    });
  }

  function onPointerDown(serial: string, e: React.PointerEvent) {
    if (!isEditing) return;

    e.preventDefault();
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    pointerStartRef.current = { x: e.clientX, y: e.clientY };
    movedRef.current = false;
    setDraggingSerial(serial);
  }

  function onPointerMove(e: React.PointerEvent) {
    if (!isEditing || !draggingSerial) return;

    const start = pointerStartRef.current;
    if (start) {
      const dx = Math.abs(e.clientX - start.x);
      const dy = Math.abs(e.clientY - start.y);
      if (dx > 4 || dy > 4) movedRef.current = true;
    }

    setNodePositionFromClientPoint(draggingSerial, e.clientX, e.clientY);
  }

  function onPointerUp(e: React.PointerEvent) {
    if (!draggingSerial) return;

    e.preventDefault();
    setDraggingSerial(null);
    pointerStartRef.current = null;
    movedRef.current = false;
  }

function handleMarkerClick(node: NodeRecord) {
  // Prevent accidental click after drag
  if (movedRef.current) return;

  if (isEditing) return;

  onNodeClick?.(node);
}

  return (
    <div
      ref={containerRef}
      className="node-map"
      onPointerMove={onPointerMove}
      onPointerUp={onPointerUp}
      onPointerCancel={onPointerUp}
    >
      <div className="node-map-toolbar">
        <div className="node-map-toolbar-hint">
          {isEditing ? "Drag to reposition" : "Positions locked"}
        </div>

        <div className="node-map-toolbar-actions">
          {!isEditing ? (
            <button
              className="node-map-text-btn"
              onClick={beginEditMode}
              type="button"
            >
              Edit
            </button>
          ) : (
            <>
              <button
                className="node-map-text-btn"
                onClick={handleSave}
                type="button"
                disabled={isSaving}
              >
                {isSaving ? "Saving..." : "Save"}
              </button>

              <button
                className="node-map-text-btn"
                onClick={handleReset}
                type="button"
                disabled={isSaving}
              >
                Reset
              </button>

              <button
                className="node-map-text-btn"
                onClick={cancelEditMode}
                type="button"
                disabled={isSaving}
              >
                Cancel
              </button>
            </>
          )}
        </div>

        {saveError ? <div className="node-map-toolbar-error">{saveError}</div> : null}
      </div>

      <div className="node-map-svg">
        <svg viewBox="0 0 300 800" xmlns="http://www.w3.org/2000/svg">
          <rect x="0" y="0" width="300" height="800" fill="transparent" />

          <polygon
            points="130,120 170,120 210,750 90,750"
            fill="#d9dde3"
            stroke="#9aa3ad"
            strokeWidth="2"
          />

          <line
            x1="150"
            y1="120"
            x2="150"
            y2="750"
            stroke="#b0b7c3"
            strokeDasharray="6,6"
          />

          <line
            x1="112"
            y1="330"
            x2="188"
            y2="330"
            stroke="#a8b2bd"
            strokeOpacity="0.55"
            strokeDasharray="5,5"
          />
          <line
            x1="101"
            y1="540"
            x2="199"
            y2="540"
            stroke="#a8b2bd"
            strokeOpacity="0.55"
            strokeDasharray="5,5"
          />

          <text x="214" y="336" fontSize="11" fill="#6d7f8e">Top</text>
          <text x="214" y="546" fontSize="11" fill="#6d7f8e">Middle</text>
          <text x="214" y="710" fontSize="11" fill="#6d7f8e">Bottom</text>

          <rect
            x="110"
            y="90"
            width="80"
            height="30"
            rx="4"
            fill="#c4c9d1"
            stroke="#9aa3ad"
          />

          <circle cx="150" cy="105" r="8" fill="#9aa3ad" />

          <line x1="150" y1="105" x2="150" y2="20" stroke="#9aa3ad" strokeWidth="4" />
          <line x1="150" y1="105" x2="60" y2="140" stroke="#9aa3ad" strokeWidth="4" />
          <line x1="150" y1="105" x2="240" y2="140" stroke="#9aa3ad" strokeWidth="4" />

          <rect x="70" y="750" width="160" height="20" fill="#b4bbc5" />
        </svg>
      </div>

      {sortedNodes.map((node) => {
        const pos = draftPositions[node.serial];
        if (!pos) return null;

        const isDragging = draggingSerial === node.serial;
        const hasWarning = node.online && warningSet.has(node.serial);

        const stateClass = !node.online
          ? "offline"
          : hasWarning
            ? "warning"
            : "online";

        return (
          <button
            key={node.serial}
            className={[
              "node-marker",
              stateClass,
              isDragging ? "dragging" : "",
              isEditing ? "editable" : "locked",
            ].join(" ")}
            style={{
              left: `${pos.x * 100}%`,
              top: `${pos.y * 100}%`,
            }}
            onPointerDown={(e) => onPointerDown(node.serial, e)}
            onClick={() => handleMarkerClick(node)}
            title={`${node.label}${node.position_zone ? ` • ${node.position_zone}` : ""}`}
            type="button"
          >
            N{node.node_id}
          </button>
        );
      })}
    </div>
  );
}