import { useMemo } from "react";
import "./SensorFilter.css";

export type SensorValue = "accelerometer" | "inclinometer" | "temperature";

export type SensorOption = { label: string; value: SensorValue };
export type TimeframeOption = { label: string; minutes: number };
export type ChannelOption = { label: string; value: string };

type Props = {
  sensorOptions: SensorOption[];
  timeframeOptions: TimeframeOption[];
  channelOptionsBySensor: Record<string, ChannelOption[]>;

  sensor: SensorValue;
  timeframeMin: number;
  channel: string;

  onSensorChange: (s: SensorValue) => void;
  onTimeframeChange: (minutes: number) => void;
  onChannelChange: (c: string) => void;
  onExport: () => void;
};

export default function SensorFilters({
  sensorOptions,
  timeframeOptions,
  channelOptionsBySensor,
  sensor,
  timeframeMin,
  channel,
  onSensorChange,
  onTimeframeChange,
  onChannelChange,
  onExport,
}: Props) {
  const channelOptions = useMemo(() => {
    return channelOptionsBySensor[sensor] ?? [{ label: "All", value: "all" }];
  }, [channelOptionsBySensor, sensor]);

  return (
    <div className="filters-row">
      <div className="control-box">
        <label className="control-label">Sensor</label>
        <select className="control-select" value={sensor} onChange={(e) => onSensorChange(e.target.value as SensorValue)}>
          {sensorOptions.map((opt) => (
            <option key={opt.value} value={opt.value}>
              {opt.label}
            </option>
          ))}
        </select>
      </div>

      <div className="control-box">
        <label className="control-label">Timeframe</label>
        <select className="control-select" value={timeframeMin} onChange={(e) => onTimeframeChange(Number(e.target.value))}>
          {timeframeOptions.map((opt) => (
            <option key={opt.minutes} value={opt.minutes}>
              {opt.label}
            </option>
          ))}
        </select>
      </div>

      <div className="control-box">
        <label className="control-label">Channel</label>
        <select className="control-select" value={channel} onChange={(e) => onChannelChange(e.target.value)}>
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
