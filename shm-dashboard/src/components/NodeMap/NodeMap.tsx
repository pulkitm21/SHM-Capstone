import { useEffect, useMemo, useRef, useState } from "react";
import type { NodeRecord } from "../../services/api";
import "./NodeMap.css";

type NodePos = { x: number; y: number };
type NodePosMap = Record<string, NodePos>;

const STORAGE_KEY = "shm_node_positions_v2";

function clamp(n: number, min: number, max: number) {
  return Math.max(min, Math.min(max, n));
}

function loadPositions(nodes: NodeRecord[]): NodePosMap {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) throw new Error("no data");
    const parsed = JSON.parse(raw) as Record<string, NodePos>;

    const out: NodePosMap = {};
    nodes.forEach((node) => {
      const p = parsed[node.serial];
      if (p && typeof p.x === "number" && typeof p.y === "number") {
        out[node.serial] = { x: clamp(p.x, 0, 1), y: clamp(p.y, 0, 1) };
      }
    });
    return out;
  } catch {
    return {};
  }
}

function savePositions(pos: NodePosMap) {
  localStorage.setItem(STORAGE_KEY, JSON.stringify(pos));
}

type NodeMapProps = {
  nodes: NodeRecord[];
  onNodeClick?: (node: NodeRecord) => void;
};

export default function NodeMap({ nodes, onNodeClick }: NodeMapProps) {
  const sortedNodes = useMemo(
    () => [...nodes].sort((a, b) => a.node_id - b.node_id),
    [nodes]
  );

  const containerRef = useRef<HTMLDivElement | null>(null);
  const pointerStartRef = useRef<{ x: number; y: number } | null>(null);
  const movedRef = useRef(false);

  const [positions, setPositions] = useState<NodePosMap>(() => loadPositions(sortedNodes));
  const [draggingSerial, setDraggingSerial] = useState<string | null>(null);

  useEffect(() => {
    setPositions((prev) => {
      const next: NodePosMap = { ...prev };
      let changed = false;

      sortedNodes.forEach((node, idx) => {
        if (!next[node.serial]) {
          const y = 0.20 + idx * (0.65 / Math.max(1, sortedNodes.length - 1));
          next[node.serial] = { x: 0.50, y: clamp(y, 0.12, 0.90) };
          changed = true;
        }
      });

      if (changed) savePositions(next);
      return next;
    });
  }, [sortedNodes]);

  function setNodePositionFromClientPoint(serial: string, clientX: number, clientY: number) {
    const el = containerRef.current;
    if (!el) return;

    const rect = el.getBoundingClientRect();
    const x = clamp((clientX - rect.left) / rect.width, 0, 1);
    const y = clamp((clientY - rect.top) / rect.height, 0, 1);

    setPositions((prev) => {
      const next = { ...prev, [serial]: { x, y } };
      savePositions(next);
      return next;
    });
  }

  function onPointerDown(serial: string, e: React.PointerEvent) {
    e.preventDefault();
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    pointerStartRef.current = { x: e.clientX, y: e.clientY };
    movedRef.current = false;
    setDraggingSerial(serial);
  }

  function onPointerMove(e: React.PointerEvent) {
    if (!draggingSerial) return;

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

    const clickedNode = sortedNodes.find((n) => n.serial === draggingSerial) ?? null;
    const wasMoved = movedRef.current;

    setDraggingSerial(null);
    pointerStartRef.current = null;
    movedRef.current = false;

    if (!wasMoved && clickedNode && onNodeClick) {
      onNodeClick(clickedNode);
    }
  }

  return (
    <div
      ref={containerRef}
      className="node-map"
      onPointerMove={onPointerMove}
      onPointerUp={onPointerUp}
      onPointerCancel={onPointerUp}
    >
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
        const pos = positions[node.serial];
        if (!pos) return null;

        return (
          <button
            key={node.serial}
            className={`node-marker ${draggingSerial === node.serial ? "dragging" : ""} ${node.online ? "online" : "offline"}`}
            style={{
              left: `${pos.x * 100}%`,
              top: `${pos.y * 100}%`,
            }}
            onPointerDown={(e) => onPointerDown(node.serial, e)}
            title={node.label}
            type="button"
          >
            N{node.node_id}
          </button>
        );
      })}
    </div>
  );
}