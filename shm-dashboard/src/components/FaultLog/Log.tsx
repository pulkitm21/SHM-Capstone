import "./Log.css";

type FaultEntry = {
  time: string;        // ISO or HH:MM:SS
  status: "High" | "Warning" | "Info";
  location: string;    // which sensor / node
  description: string;
};

// Static for now
const FAULT_LOG: FaultEntry[] = [
  {
    time: "12:45:33",
    status: "High",
    location: "Nacelle – A1",
    description: "High temperature alert",
  },
  {
    time: "09:20:45",
    status: "Warning",
    location: "Tower mid – B2",
    description: "Intermittent connection loss",
  },
  {
    time: "09:18:07",
    status: "High",
    location: "Foundation – C1",
    description: "Tilt angle out of range",
  },
];

export default function FaultLog() {
  return (
    <div className="fault-log">
      <table className="fault-table">
        <thead>
          <tr>
            <th>Time</th>
            <th>Status</th>
            <th>Location</th>
            <th>Description</th>
          </tr>
        </thead>

        <tbody>
          {FAULT_LOG.map((entry, idx) => (
            <tr key={idx}>
              <td className="mono">{entry.time}</td>
              <td>
                <span className={`status-pill ${entry.status.toLowerCase()}`}>
                  {entry.status}
                </span>
              </td>
              <td>{entry.location}</td>
              <td>{entry.description}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
