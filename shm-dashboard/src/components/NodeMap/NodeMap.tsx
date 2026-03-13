import { useEffect, useMemo, useRef, useState } from "react";
import {
  putNodePosition,
  type NodeRecord,
} from "../../services/api";
import "./NodeMap.css";

type NodePos = { x: number; y: number };
type NodePosMap = Record<string, NodePos>;

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
        x: clamp(node.x as number, 0, 1),
        y: clamp(node.y as number, 0, 1),
      };
      return;
    }

    const y = 0.20 + idx * (0.65 / Math.max(1, nodes.length - 1));
    out[node.serial] = { x: 0.50, y: clamp(y, 0.12, 0.90) };
  });

  return out;
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
  const saveTimeoutRef = useRef<number | null>(null);

  const [positions, setPositions] = useState<NodePosMap>(() => buildPositions(sortedNodes));
  const [draggingSerial, setDraggingSerial] = useState<string | null>(null);
  const [savingSerial, setSavingSerial] = useState<string | null>(null);

  useEffect(() => {
    setPositions(buildPositions(sortedNodes));
  }, [sortedNodes]);

  useEffect(() => {
    return () => {
      if (saveTimeoutRef.current !== null) {
        window.clearTimeout(saveTimeoutRef.current);
      }
    };
  }, []);

  async function persistNodePosition(node: NodeRecord, pos: NodePos) {
    try {
      setSavingSerial(node.serial);
      await putNodePosition(node.node_id, {
        x: clamp(pos.x, 0, 1),
        y: clamp(pos.y, 0, 1),
      });
    } catch (error) {
      console.error("Failed to save node position:", error);
    } finally {
      setSavingSerial((current) => (current === node.serial ? null : current));
    }
  }

  function schedulePositionSave(node: NodeRecord, pos: NodePos) {
    if (saveTimeoutRef.current !== null) {
      window.clearTimeout(saveTimeoutRef.current);
    }

    saveTimeoutRef.current = window.setTimeout(() => {
      persistNodePosition(node, pos);
    }, 150);
  }

  function setNodePositionFromClientPoint(serial: string, clientX: number, clientY: number) {
    const el = containerRef.current;
    if (!el) return;

    const rect = el.getBoundingClientRect();
    const x = clamp((clientX - rect.left) / rect.width, 0, 1);
    const y = clamp((clientY - rect.top) / rect.height, 0, 1);

    setPositions((prev) => ({ ...prev, [serial]: { x, y } }));
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
    const finalPos = positions[draggingSerial];
    const wasMoved = movedRef.current;

    setDraggingSerial(null);
    pointerStartRef.current = null;
    movedRef.current = false;

    if (wasMoved && clickedNode && finalPos) {
      schedulePositionSave(clickedNode, finalPos);
      return;
    }

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

        const isDragging = draggingSerial === node.serial;
        const isSaving = savingSerial === node.serial;

        return (
          <button
            key={node.serial}
            className={`node-marker ${isDragging ? "dragging" : ""} ${node.online ? "online" : "offline"}`}
            style={{
              left: `${pos.x * 100}%`,
              top: `${pos.y * 100}%`,
            }}
            onPointerDown={(e) => onPointerDown(node.serial, e)}
            title={isSaving ? `${node.label} (saving...)` : node.label}
            type="button"
          >
            N{node.node_id}
          </button>
        );
      })}
    </div>
  );
}