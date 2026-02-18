import {
  Chart as ChartJS,
  LineElement,
  PointElement,
  LinearScale,
  CategoryScale,
  Tooltip,
  Legend,
  Filler,
} from "chart.js";
import { Line } from "react-chartjs-2";

ChartJS.register(LineElement, PointElement, LinearScale, CategoryScale, Tooltip, Legend, Filler);

export type SensorPoint = { ts: string; value: number };

type Props = {
  title: string;           
  sensorKey: string;             
  unit?: string;                 
  points: SensorPoint[];         
  height?: number;               
};

export default function SensorLineChart({
  title,
  sensorKey,
  unit = "",
  points,
  height = 320,
}: Props) {
  const labels = points.map((p) => p.ts.slice(11, 16)); // HH:MM
  const values = points.map((p) => p.value);

  const data = {
    labels,
    datasets: [
      {
        label: unit ? `${sensorKey} (${unit})` : sensorKey,
        data: values,
        tension: 0.25,
        pointRadius: 0,
        fill: false,
      },
    ],
  };

  const options = {
    responsive: true,
    maintainAspectRatio: false as const,
    plugins: {
      legend: { display: true },
      tooltip: { enabled: true },
      title: {
        display: true,
        text: title,
      },
    },
    scales: {
      x: {
        ticks: { maxTicksLimit: 8 },
      },
      y: {
        beginAtZero: false,
        title: unit ? { display: true, text: unit } : { display: false, text: "" },
      },
    },
  };

  return (
    <div style={{ width: "100%", height }}>
      <Line data={data} options={options} />
    </div>
  );
}
