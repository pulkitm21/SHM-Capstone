// Draggable node points over turbine diagram to visually represent positions
// positions saved to localStorage need to save to settings on backend

import { useEffect, useMemo, useRef, useState } from "react";
import "./NodeMap.css";

type NodePos = { x: number; y: number };
type NodePosMap = Record<number, NodePos>;

const STORAGE_KEY = "shm_node_positions";

function clamp(n: number, min: number, max: number) {
  return Math.max(min, Math.min(max, n));
}

function loadPositions(nodeIds: number[]): NodePosMap {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) throw new Error("no data");
    const parsed = JSON.parse(raw) as Record<string, NodePos>;

    const out: NodePosMap = {};
    nodeIds.forEach((id) => {
      const p = parsed[String(id)];
      if (p && typeof p.x === "number" && typeof p.y === "number") {
        out[id] = { x: clamp(p.x, 0, 1), y: clamp(p.y, 0, 1) };
      }
    });
    return out;
  } catch {
    return {};
  }
}

function savePositions(pos: NodePosMap) {
  const serializable: Record<string, NodePos> = {};
  Object.entries(pos).forEach(([k, v]) => {
    serializable[k] = { x: v.x, y: v.y };
  });
  localStorage.setItem(STORAGE_KEY, JSON.stringify(serializable));
}

export default function NodeMap(props: { nodeCount?: number }) {
  const nodeCount = props.nodeCount ?? 3; // default to 3 nodes if not specified
  const nodeIds = useMemo(() => Array.from({ length: nodeCount }, (_, i) => i + 1), [nodeCount]);

  const containerRef = useRef<HTMLDivElement | null>(null);

  const [positions, setPositions] = useState<NodePosMap>(() => loadPositions(nodeIds));
  const [draggingId, setDraggingId] = useState<number | null>(null);

  // If some nodes have no saved position yet, give them a sensible default down the tower -> should be place in the bottom right wiaitng to be dragged and dropped
  useEffect(() => {
    setPositions((prev) => {
      const next: NodePosMap = { ...prev };
      let changed = false;

      nodeIds.forEach((id, idx) => {
        if (!next[id]) {
          // default: centered on tower, staggered vertically
          const y = 0.20 + idx * (0.65 / Math.max(1, nodeIds.length - 1));
          next[id] = { x: 0.50, y: clamp(y, 0.12, 0.90) };
          changed = true;
        }
      });

      if (changed) savePositions(next);
      return next;
    });
  }, [nodeIds]);

  function setNodePositionFromClientPoint(nodeId: number, clientX: number, clientY: number) {
    const el = containerRef.current;
    if (!el) return;

    const rect = el.getBoundingClientRect();

    // convert pointer position into normalized coords (0..1)
    const x = clamp((clientX - rect.left) / rect.width, 0, 1);
    const y = clamp((clientY - rect.top) / rect.height, 0, 1);

    setPositions((prev) => {
      const next = { ...prev, [nodeId]: { x, y } };
      savePositions(next);
      return next;
    });
  }

  function onPointerDown(nodeId: number, e: React.PointerEvent) {
    e.preventDefault();
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    setDraggingId(nodeId);
  }

  function onPointerMove(e: React.PointerEvent) {
    if (draggingId === null) return;
    setNodePositionFromClientPoint(draggingId, e.clientX, e.clientY);
  }

  function onPointerUp(e: React.PointerEvent) {
    if (draggingId === null) return;
    e.preventDefault();
    setDraggingId(null);
  }

  return (
    <div
      ref={containerRef}
      className="node-map"
      onPointerMove={onPointerMove}
      onPointerUp={onPointerUp}
      onPointerCancel={onPointerUp}
    >
      {/* Turbine diagram */}
      <div className="node-map-svg">
        <svg viewBox="0 0 300 800" xmlns="http://www.w3.org/2000/svg">
          <rect x="0" y="0" width="300" height="800" fill="transparent" />

          {/* Tower */}
          <polygon
            points="130,120 170,120 210,750 90,750"
            fill="#e6e9ef"
            stroke="#c0c6cf"
            strokeWidth="2"
          />

          {/* Nacelle */}
          <rect
            x="110"
            y="90"
            width="80"
            height="30"
            rx="4"
            fill="#c4c9d1"
            stroke="#9aa3ad"
          />

          {/* Hub */}
          <circle cx="150" cy="105" r="8" fill="#9aa3ad" />

          {/* Blades */}
          <line x1="150" y1="105" x2="150" y2="20" stroke="#9aa3ad" strokeWidth="4" />
          <line x1="150" y1="105" x2="60" y2="140" stroke="#9aa3ad" strokeWidth="4" />
          <line x1="150" y1="105" x2="240" y2="140" stroke="#9aa3ad" strokeWidth="4" />

          {/* Base */}
          <rect x="70" y="750" width="160" height="20" fill="#b4bbc5" />
        </svg>
      </div>

      {/* Node markers overlay */}
      {nodeIds.map((id) => {
        const pos = positions[id];
        if (!pos) return null;

        return (
          <button
            key={id}
            className={`node-marker ${draggingId === id ? "dragging" : ""}`}
            style={{
              left: `${pos.x * 100}%`,
              top: `${pos.y * 100}%`,
            }}
            onPointerDown={(e) => onPointerDown(id, e)}
            title={`Node ${id}`}
            type="button"
          >
            N{id}
          </button>
        );
      })}
    </div>
  );
}