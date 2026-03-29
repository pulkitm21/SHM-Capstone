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
import { Scatter } from "react-chartjs-2";

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
  const datasets =
    data.sensor === "accelerometer"
      ? buildAccelerometerDatasets(data.points)
      : data.sensor === "inclinometer"
      ? buildInclinometerDatasets(data.points)
      : buildTemperatureDatasets(data.points, data.unit ?? "");

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

  return (
    <div style={{ width: "100%", height }}>
      <Scatter data={chartData} options={options} />
    </div>
  );
}
