import {
  Chart as ChartJS,
  Filler,
  Legend,
  LinearScale,
  PointElement,
  Tooltip,
  type ChartData,
  type ChartOptions,
} from "chart.js";
import { useEffect, useMemo, useState } from "react";
import { Scatter } from "react-chartjs-2";

import "./SensorPlot.css";

import type {
  AccelerometerPlotPoint,
  ApiResponse,
  InclinometerPlotPoint,
  TemperaturePlotPoint,
} from "../../services/api";

ChartJS.register(
  PointElement,
  LinearScale,
  Tooltip,
  Legend,
  Filler
);

type Props = {
  title: string;
  data: ApiResponse;
  height?: number;
};

type ScatterPoint = {
  x: number;
  y: number;
  ts: string;
};

function formatTimeLabel(ts: string): string {
  const date = new Date(ts);
  if (Number.isNaN(date.getTime())) return ts;
  return date.toLocaleTimeString([], {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
}

function buildAxisScatterData<T extends { ts: string }>(
  points: T[],
  getValue: (point: T) => number | null
): ScatterPoint[] {
  return points.flatMap((point, index) => {
    const value = getValue(point);
    if (value == null) return [];
    return [
      {
        x: index,
        y: value,
        ts: point.ts,
      },
    ];
  });
}

function buildAccelerometerDatasets(points: AccelerometerPlotPoint[]) {
  return [
    {
      label: "X",
      data: buildAxisScatterData(points, (p) => p.x),
      borderColor: "#2563eb",
      backgroundColor: "#2563eb",
      pointRadius: 2,
      pointHoverRadius: 3,
      showLine: false,
    },
    {
      label: "Y",
      data: buildAxisScatterData(points, (p) => p.y),
      borderColor: "#059669",
      backgroundColor: "#059669",
      pointRadius: 2,
      pointHoverRadius: 3,
      showLine: false,
    },
    {
      label: "Z",
      data: buildAxisScatterData(points, (p) => p.z),
      borderColor: "#dc2626",
      backgroundColor: "#dc2626",
      pointRadius: 2,
      pointHoverRadius: 3,
      showLine: false,
    },
  ];
}

function buildInclinometerDatasets(points: InclinometerPlotPoint[]) {
  return [
    {
      label: "Roll",
      data: buildAxisScatterData(points, (p) => p.roll),
      borderColor: "#7c3aed",
      backgroundColor: "#7c3aed",
      pointRadius: 2,
      pointHoverRadius: 3,
      showLine: false,
    },
    {
      label: "Pitch",
      data: buildAxisScatterData(points, (p) => p.pitch),
      borderColor: "#ea580c",
      backgroundColor: "#ea580c",
      pointRadius: 2,
      pointHoverRadius: 3,
      showLine: false,
    },
    {
      label: "Yaw",
      data: buildAxisScatterData(points, (p) => p.yaw),
      borderColor: "#0891b2",
      backgroundColor: "#0891b2",
      pointRadius: 2,
      pointHoverRadius: 3,
      showLine: false,
    },
  ];
}

function buildTemperatureDatasets(points: TemperaturePlotPoint[], unit: string) {
  return [
    {
      label: unit ? `Temperature (${unit})` : "Temperature",
      data: buildAxisScatterData(points, (p) => p.value),
      borderColor: "#d97706",
      backgroundColor: "#d97706",
      pointRadius: 2,
      pointHoverRadius: 3,
      showLine: false,
    },
  ];
}

export default function SensorLineChart({
  title,
  data,
  height = 420,
}: Props) {
  const channelOptions = useMemo(() => {
    if (data.sensor === "accelerometer") {
      return [
        { key: "X", label: "X" },
        { key: "Y", label: "Y" },
        { key: "Z", label: "Z" },
      ];
    }

    if (data.sensor === "inclinometer") {
      return [
        { key: "Roll", label: "Roll" },
        { key: "Pitch", label: "Pitch" },
        { key: "Yaw", label: "Yaw" },
      ];
    }

    return [];
  }, [data.sensor]);

  const [selectedChannels, setSelectedChannels] = useState<string[]>(
    channelOptions.map((option) => option.key)
  );

  // Reset channel selection when the plotted sensor type changes.
  useEffect(() => {
    setSelectedChannels(channelOptions.map((option) => option.key));
  }, [channelOptions]);

  const allDatasets =
    data.sensor === "accelerometer"
      ? buildAccelerometerDatasets(data.points)
      : data.sensor === "inclinometer"
      ? buildInclinometerDatasets(data.points)
      : buildTemperatureDatasets(data.points, data.unit ?? "");

  const datasets = allDatasets.filter((dataset) => {
    if (!channelOptions.length) return true;
    return selectedChannels.includes(String(dataset.label));
  });

  const chartData: ChartData<"scatter"> = {
    datasets,
  };

  const options: ChartOptions<"scatter"> = {
    responsive: true,
    maintainAspectRatio: false,
    parsing: false,
    interaction: {
      mode: "nearest",
      intersect: false,
    },
    plugins: {
      legend: {
        display: true,
        position: "top",
      },
      tooltip: {
        enabled: true,
        callbacks: {
          title(items) {
            const raw = items[0]?.raw as ScatterPoint | undefined;
            return raw ? formatTimeLabel(raw.ts) : "";
          },
        },
      },
      title: {
        display: true,
        text: title,
      },
    },
    scales: {
      x: {
        type: "linear",
        ticks: {
          maxTicksLimit: 8,
          callback(value) {
            const index = Math.round(Number(value));
            const point = data.points[index];
            return point ? formatTimeLabel(point.ts) : "";
          },
        },
        title: {
          display: true,
          text: "Time",
        },
      },
      y: {
        beginAtZero: false,
        title: {
          display: true,
          text: data.unit ?? "",
        },
      },
    },
  };

  function handleChannelToggle(channel: string) {
    setSelectedChannels((current) => {
      if (current.includes(channel)) {
        // Keep at least one channel selected so the plot never goes blank by accident.
        if (current.length === 1) return current;
        return current.filter((value) => value !== channel);
      }

      return [...current, channel];
    });
  }

  return (
    <div className="sp-chart-shell">
      {channelOptions.length > 0 && (
        <div className="sp-channel-toolbar">
          <span className="sp-channel-label">Channels</span>

          <div className="sp-channel-list">
            {channelOptions.map((option) => (
              <label key={option.key} className="sp-channel-option">
                <input
                  type="checkbox"
                  checked={selectedChannels.includes(option.key)}
                  onChange={() => handleChannelToggle(option.key)}
                />
                <span>{option.label}</span>
              </label>
            ))}
          </div>
        </div>
      )}

      <div style={{ width: "100%", height }}>
        <Scatter data={chartData} options={options} />
      </div>
    </div>
  );
}
