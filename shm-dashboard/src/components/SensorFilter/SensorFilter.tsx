import "./SensorFilter.css";

// This component does NOT perform any data filtering.
// It only provides UI controls for timeframe/channel + export.

export type SensorValue = "accelerometer" | "inclinometer" | "temperature";

export type TimeframeOption = { label: string; minutes: number };
export type ChannelOption = { label: string; value: string };

type Props = {
  timeframeOptions: TimeframeOption[];
  channelOptionsBySensor: Record<string, ChannelOption[]>;

  sensor: SensorValue;
  timeframeMin: number;
  channel: string;

  onTimeframeChange: (minutes: number) => void;
  onChannelChange: (c: string) => void;
  onExport: () => void;
};

export default function SensorFilters({
  timeframeOptions,
  channelOptionsBySensor,
  sensor,
  timeframeMin,
  channel,
  onTimeframeChange,
  onChannelChange,
  onExport,
}: Props) {
  const channelOptions =
    channelOptionsBySensor[sensor] ?? [{ label: "All", value: "all" }];

  return (
    <div className="filters-row">
      <div className="control-box">
        <label className="control-label">Timeframe</label>
        <select
          className="control-select"
          value={timeframeMin}
          onChange={(e) => onTimeframeChange(Number(e.target.value))}
        >
          {timeframeOptions.map((opt) => (
            <option key={opt.minutes} value={opt.minutes}>
              {opt.label}
            </option>
          ))}
        </select>
      </div>

      <div className="control-box">
        <label className="control-label">Channel</label>
        <select
          className="control-select"
          value={channel}
          onChange={(e) => onChannelChange(e.target.value)}
        >
          {channelOptions.map((opt) => (
            <option key={opt.value} value={opt.value}>
              {opt.label}
            </option>
          ))}
        </select>
      </div>

      <div className="control-box export-box">
        <label className="control-label">Export</label>
        <button className="export-button" onClick={onExport}>
          Export
        </button>
      </div>
    </div>
  );
}