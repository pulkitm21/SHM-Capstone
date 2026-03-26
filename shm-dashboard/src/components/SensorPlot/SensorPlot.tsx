import {
  Chart as ChartJS,
  CategoryScale,
  Filler,
  Legend,
  LineElement,
  LinearScale,
  PointElement,
  Tooltip,
  type ChartData,
  type ChartOptions,
} from "chart.js";
import { Line } from "react-chartjs-2";

import type {
  AccelerometerPlotPoint,
  ApiResponse,
  InclinometerPlotPoint,
  TemperaturePlotPoint,
} from "../../services/api";

ChartJS.register(
  LineElement,
  PointElement,
  LinearScale,
  CategoryScale,
  Tooltip,
  Legend,
  Filler
);

type Props = {
  title: string;
  data: ApiResponse;
  height?: number;
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

function buildAccelerometerDatasets(points: AccelerometerPlotPoint[]) {
  return [
    {
      label: "X",
      data: points.map((p) => p.x),
      borderColor: "#2563eb",
      backgroundColor: "#2563eb",
      tension: 0.2,
      pointRadius: 0,
      borderWidth: 2,
      fill: false,
    },
    {
      label: "Y",
      data: points.map((p) => p.y),
      borderColor: "#059669",
      backgroundColor: "#059669",
      tension: 0.2,
      pointRadius: 0,
      borderWidth: 2,
      fill: false,
    },
    {
      label: "Z",
      data: points.map((p) => p.z),
      borderColor: "#dc2626",
      backgroundColor: "#dc2626",
      tension: 0.2,
      pointRadius: 0,
      borderWidth: 2,
      fill: false,
    },
  ];
}

function buildInclinometerDatasets(points: InclinometerPlotPoint[]) {
  return [
    {
      label: "Roll",
      data: points.map((p) => p.roll),
      borderColor: "#7c3aed",
      backgroundColor: "#7c3aed",
      tension: 0.2,
      pointRadius: 0,
      borderWidth: 2,
      fill: false,
    },
    {
      label: "Pitch",
      data: points.map((p) => p.pitch),
      borderColor: "#ea580c",
      backgroundColor: "#ea580c",
      tension: 0.2,
      pointRadius: 0,
      borderWidth: 2,
      fill: false,
    },
    {
      label: "Yaw",
      data: points.map((p) => p.yaw),
      borderColor: "#0891b2",
      backgroundColor: "#0891b2",
      tension: 0.2,
      pointRadius: 0,
      borderWidth: 2,
      fill: false,
    },
  ];
}

function buildTemperatureDatasets(points: TemperaturePlotPoint[], unit: string) {
  return [
    {
      label: unit ? `Temperature (${unit})` : "Temperature",
      data: points.map((p) => p.value),
      borderColor: "#d97706",
      backgroundColor: "#d97706",
      tension: 0.2,
      pointRadius: 0,
      borderWidth: 2,
      fill: false,
    },
  ];
}

export default function SensorLineChart({
  title,
  data,
  height = 420,
}: Props) {
  const labels = data.points.map((p) => formatTimeLabel(p.ts));

  const datasets =
    data.sensor === "accelerometer"
      ? buildAccelerometerDatasets(data.points)
      : data.sensor === "inclinometer"
      ? buildInclinometerDatasets(data.points)
      : buildTemperatureDatasets(data.points, data.unit ?? "");

  const chartData: ChartData<"line"> = {
    labels,
    datasets,
  };

  const options: ChartOptions<"line"> = {
    responsive: true,
    maintainAspectRatio: false,
    interaction: {
      mode: "index",
      intersect: false,
    },
    plugins: {
      legend: {
        display: true,
        position: "top",
      },
      tooltip: {
        enabled: true,
      },
      title: {
        display: true,
        text: title,
      },
    },
    scales: {
      x: {
        ticks: { maxTicksLimit: 8 },
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
      <Line data={chartData} options={options} />
    </div>
  );
}