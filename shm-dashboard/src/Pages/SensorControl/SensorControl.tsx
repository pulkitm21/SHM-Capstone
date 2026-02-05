import { useEffect, useMemo, useState } from "react";
import { useNavigate } from "react-router-dom";

import SystemStatus from "../../components/SystemStatus/SystemStatus";
import SensorInfoCard, { type SensorMeta } from "../../components/SensorInfo/SensorInfo";
import SensorConfigCard from "../../components/SensorConfig/SensorConfig";
import FaultLog from "../../components/FaultLog/Log";

import SensorFilters, { type SensorValue } from "../../components/SensorFilter/SensorFilter";

import SensorLineChart from "../../components/SensorPlot/SensorPlot";
import type { SensorPoint } from "../../components/SensorPlot/SensorPlot";

import "./SensorControl.css";

const SENSOR_OPTIONS = [
  { label: "Accelerometer", value: "accelerometer" },
  { label: "Inclinometer", value: "inclinometer" },
  { label: "Temperature", value: "temperature" },
] as const;

const TIMEFRAME_OPTIONS = [
  { label: "1 day", minutes: 1440 },
  { label: "7 days", minutes: 10080 },
  { label: "30 days", minutes: 43200 },
] as const;

const CHANNELS_BY_SENSOR: Record<string, { label: string; value: string }[]> = {
  accelerometer: [
    { label: "All", value: "all" },
    { label: "X", value: "x" },
    { label: "Y", value: "y" },
    { label: "Z", value: "z" },
  ],
  inclinometer: [
    { label: "All", value: "all" },
    { label: "Pitch", value: "pitch" },
    { label: "Roll", value: "roll" },
  ],
  temperature: [{ label: "All", value: "all" }],
};

const SENSOR_META: Record<SensorValue, SensorMeta & { unit: string; plotKey: string }> = {
  accelerometer: {
    model: "ADXL355",
    serial: "SN00023",
    installationDate: "2024-03-15",
    location: "Tower",
    orientation: "+X +Y +Z",
    unit: "g",
    plotKey: "accelerometer",
  },
  inclinometer: {
    model: "SCL3300",
    serial: "SN00110",
    installationDate: "2024-03-15",
    location: "Foundation",
    orientation: "Pitch/Roll",
    unit: "deg",
    plotKey: "inclinometer",
  },
  temperature: {
    model: "ADT7420",
    serial: "SN00402",
    installationDate: "2024-03-15",
    location: "Tower",
    orientation: "N/A",
    unit: "°C",
    plotKey: "temperature",
  },
};

function isoMinutesAgo(minAgo: number) {
  const d = new Date(Date.now() - minAgo * 60_000);
  return d.toISOString();
}

function generateDummySeries(sensor: SensorValue, channel: string, minutes: number): SensorPoint[] {
  const stepMin = minutes <= 1440 ? 5 : minutes <= 10080 ? 30 : 60;
  const n = Math.floor(minutes / stepMin);

  const base =
    sensor === "temperature" ? 22 :
    sensor === "inclinometer" ? 2 :
    0.2;

  const channelOffset =
    channel === "x" ? 0.02 :
    channel === "y" ? -0.01 :
    channel === "z" ? 0.01 :
    channel === "pitch" ? 0.15 :
    channel === "roll" ? -0.12 :
    0;

  const pts: SensorPoint[] = [];
  for (let i = 0; i <= n; i++) {
    const minAgo = minutes - i * stepMin;
    const trend = (i / Math.max(1, n)) * (sensor === "temperature" ? 1.5 : 0.2);
    const wave = Math.sin(i / 6) * (sensor === "inclinometer" ? 0.4 : 0.08);
    const noise = (Math.random() - 0.5) * (sensor === "temperature" ? 0.6 : 0.12);

    pts.push({ ts: isoMinutesAgo(minAgo), value: base + channelOffset + trend + wave + noise });
  }
  return pts;
}

export default function SensorControl() {
  const navigate = useNavigate();

  // static for now
  const isOnline = true;

  const [sensor, setSensor] = useState<SensorValue>("accelerometer");
  const [timeframeMin, setTimeframeMin] = useState<number>(1440);

  const channelOptions = useMemo(() => CHANNELS_BY_SENSOR[sensor] ?? [{ label: "All", value: "all" }], [sensor]);
  const [channel, setChannel] = useState<string>("all");

  // reset channel safely when sensor changes
  useEffect(() => {
    setChannel("all");
  }, [sensor]);

  const meta = SENSOR_META[sensor];

  const [points, setPoints] = useState<SensorPoint[]>([]);
  useEffect(() => {
    setPoints(generateDummySeries(sensor, channel, timeframeMin));
  }, [sensor, channel, timeframeMin]);

  return (
    <div className="sc-page">
      <SystemStatus isOnline={isOnline} />

      <div className="sc-top-cards">
        <SensorInfoCard meta={meta} />
        <SensorConfigCard />
        <div className="sc-card">
          <div className="sc-card-title">Fault Log</div>
          <div className="sc-card-body">
            <FaultLog />
          </div>
        </div>
      </div>

      <SensorFilters
        sensorOptions={[...SENSOR_OPTIONS]}
        timeframeOptions={[...TIMEFRAME_OPTIONS]}
        channelOptionsBySensor={CHANNELS_BY_SENSOR}
        sensor={sensor}
        timeframeMin={timeframeMin}
        channel={channel}
        onSensorChange={setSensor}
        onTimeframeChange={setTimeframeMin}
        onChannelChange={setChannel}
        onExport={() => navigate("/export")}
      />

      <div className="sc-plot-card">
        <SensorLineChart
          title={SENSOR_OPTIONS.find((s) => s.value === sensor)?.label ?? "Sensor"}
          sensorKey={meta.plotKey}
          unit={meta.unit}
          points={points}
          height={420}
        />
      </div>
    </div>
  );
}
