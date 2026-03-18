import type { FaultRow, NodeRecord } from "../../services/api";
import "./NodeTable.css";

type Props = {
  nodes: NodeRecord[];
  selectedNodeLabel: string;
  onSelectNode: (label: string) => void;
  faultsBySerial: Record<string, FaultRow[]>;
};

function formatTimestamp(ts?: string) {
  if (!ts) return "—";
  const date = new Date(ts);
  if (Number.isNaN(date.getTime())) return ts;
  return date.toLocaleString();
}

function countUnresolvedFaultsForSerial(faults: FaultRow[], serial: string) {
  return faults.filter(
    (f) =>
      f.serial_number === serial &&
      String(f.fault_status || "").toLowerCase() === "active"
  ).length;
}

export default function NodeTable({
  nodes,
  selectedNodeLabel,
  onSelectNode,
  faultsBySerial,
}: Props) {
  return (
    <div className="sm-node-table-panel">
      <div className="sm-panel-header">
        <h2 className="sm-panel-title">Nodes</h2>
        <span className="sm-panel-count">{nodes.length}</span>
      </div>

      <div className="sm-node-table-wrap">
        <table className="sm-node-table">
          <thead>
            <tr>
              <th>Node</th>
              <th>Status</th>
              <th>First Seen</th>
              <th>Last Seen</th>
              <th>Unresolved Faults</th>
              <th>Action</th>
            </tr>
          </thead>

          <tbody>
            {nodes.length === 0 && (
              <tr>
                <td colSpan={6}>No nodes discovered</td>
              </tr>
            )}

            {nodes.map((node) => {
              const selected = node.label === selectedNodeLabel;
              const unresolvedFaults = countUnresolvedFaultsForSerial(
                faultsBySerial[node.serial] ?? [],
                node.serial
              );

              return (
                <tr key={node.serial} className={selected ? "selected" : ""}>
                  <td>
                    <div className="sm-node-primary">{node.serial}</div>
                    <div className="sm-node-secondary">{node.label}</div>
                  </td>

                  <td>
                    <span className={`sm-health-pill ${node.online ? "online" : "offline"}`}>
                      {node.online ? "Online" : "Offline"}
                    </span>
                  </td>

                  <td>{formatTimestamp(node.first_seen)}</td>
                  <td>{formatTimestamp(node.last_seen)}</td>
                  <td>{unresolvedFaults}</td>

                  <td>
                    <button
                      className={`sm-select-btn ${selected ? "active" : ""}`}
                      onClick={() => onSelectNode(node.label)}
                    >
                      {selected ? "Selected" : "Select"}
                    </button>
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </div>
  );
}