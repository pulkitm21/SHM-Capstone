import {
  Chart as ChartJS,
  Filler,
  Legend,
  LineElement,
  LinearScale,
  PointElement,
  Tooltip,
  type ChartData,
  type ChartOptions,
} from "chart.js";
import zoomPlugin from "chartjs-plugin-zoom";
import { memo, useEffect, useMemo, useRef, useState } from "react";
import { Line } from "react-chartjs-2";

import "./SensorPlot.css";

import type {
  AccelerometerPlotPoint,
  ApiResponse,
  InclinometerPlotPoint,
  TemperaturePlotPoint,
} from "../../services/api";

ChartJS.register(
  PointElement,
  LineElement,
  LinearScale,
  Tooltip,
  Legend,
  Filler,
  zoomPlugin
);

type Props = {
  title: string;
  data: ApiResponse;
  height?: number;
  onZoomStateChange?: (zoomed: boolean) => void;
};

type PlotPoint = {
  x: number;
  y: number;
  ts: string;
};

type SensorDataset = {
  label: string;
  data: PlotPoint[];
  borderColor: string;
  backgroundColor: string;
  pointRadius: number;
  pointHoverRadius: number;
  borderWidth: number;
  tension: number;
  fill: boolean;
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

function buildAxisPlotData<T extends { ts: string }>(
  points: T[],
  getValue: (point: T) => number | null
): PlotPoint[] {
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

function buildAccelerometerDatasets(points: AccelerometerPlotPoint[]): SensorDataset[] {
  return [
    {
      label: "X",
      data: buildAxisPlotData(points, (p) => p.x),
      borderColor: "#2563eb",
      backgroundColor: "#2563eb",
      pointRadius: 0,
      pointHoverRadius: 3,
      borderWidth: 2,
      tension: 0,
      fill: false,
    },
    {
      label: "Y",
      data: buildAxisPlotData(points, (p) => p.y),
      borderColor: "#059669",
      backgroundColor: "#059669",
      pointRadius: 0,
      pointHoverRadius: 3,
      borderWidth: 2,
      tension: 0,
      fill: false,
    },
    {
      label: "Z",
      data: buildAxisPlotData(points, (p) => p.z),
      borderColor: "#dc2626",
      backgroundColor: "#dc2626",
      pointRadius: 0,
      pointHoverRadius: 3,
      borderWidth: 2,
      tension: 0,
      fill: false,
    },
  ];
}

function buildInclinometerDatasets(points: InclinometerPlotPoint[]): SensorDataset[] {
  return [
    {
      label: "Roll",
      data: buildAxisPlotData(points, (p) => p.roll),
      borderColor: "#7c3aed",
      backgroundColor: "#7c3aed",
      pointRadius: 0,
      pointHoverRadius: 3,
      borderWidth: 2,
      tension: 0,
      fill: false,
    },
    {
      label: "Pitch",
      data: buildAxisPlotData(points, (p) => p.pitch),
      borderColor: "#ea580c",
      backgroundColor: "#ea580c",
      pointRadius: 0,
      pointHoverRadius: 3,
      borderWidth: 2,
      tension: 0,
      fill: false,
    },
    {
      label: "Yaw",
      data: buildAxisPlotData(points, (p) => p.yaw),
      borderColor: "#0891b2",
      backgroundColor: "#0891b2",
      pointRadius: 0,
      pointHoverRadius: 3,
      borderWidth: 2,
      tension: 0,
      fill: false,
    },
  ];
}

function buildTemperatureDatasets(
  points: TemperaturePlotPoint[],
  unit: string
): SensorDataset[] {
  return [
    {
      label: unit ? `Temperature (${unit})` : "Temperature",
      data: buildAxisPlotData(points, (p) => p.value),
      borderColor: "#d97706",
      backgroundColor: "#d97706",
      pointRadius: 0,
      pointHoverRadius: 3,
      borderWidth: 2,
      tension: 0,
      fill: false,
    },
  ];
}

function buildChartOptions(
  points: { ts: string }[],
  yAxisLabel: string,
  onViewportChange?: (zoomed: boolean) => void
): ChartOptions<"line"> {
  const maxIndex = Math.max(points.length - 1, 1);
  const minRange = Math.max(Math.min(points.length - 1, 20), 1);

  return {
    responsive: true,
    maintainAspectRatio: false,
    parsing: false,
    interaction: {
      mode: "nearest",
      intersect: false,
    },
    plugins: {
      legend: {
        display: false,
      },
      tooltip: {
        enabled: true,
        callbacks: {
          title(items) {
            const raw = items[0]?.raw as PlotPoint | undefined;
            return raw ? formatTimeLabel(raw.ts) : "";
          },
        },
      },
      zoom: {
        limits: {
          x: {
            min: 0,
            max: maxIndex,
            minRange,
          },
        },
        pan: {
          enabled: true,
          mode: "x",
          modifierKey: "shift",
          onPanStart() {
            onViewportChange?.(true);
          },
        },
        zoom: {
          mode: "x",
          wheel: {
            enabled: true,
            modifierKey: "ctrl",
            speed: 0.08,
          },
          drag: {
            enabled: true,
            modifierKey: "alt",
            backgroundColor: "rgba(37, 99, 235, 0.10)",
            borderColor: "rgba(37, 99, 235, 0.35)",
            borderWidth: 1,
          },
          onZoomStart() {
            onViewportChange?.(true);
          },
        },
      },
    },
    scales: {
      x: {
        type: "linear",
        ticks: {
          maxTicksLimit: 8,
          callback(value) {
            const index = Math.round(Number(value));
            const point = points[index];
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
          text: yAxisLabel,
        },
      },
    },
  };
}

function SensorLineChart({
  title,
  data,
  height = 420,
  onZoomStateChange,
}: Props) {
  const chartRefs = useRef<Record<string, ChartJS<"line"> | null>>({});
  const [chartZoomState, setChartZoomState] = useState<Record<string, boolean>>({});

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

  useEffect(() => {
    setSelectedChannels(channelOptions.map((option) => option.key));
  }, [channelOptions]);

  const allDatasets =
    data.sensor === "accelerometer"
      ? buildAccelerometerDatasets(data.points)
      : data.sensor === "inclinometer"
        ? buildInclinometerDatasets(data.points)
        : buildTemperatureDatasets(data.points, data.unit ?? "");

  const visibleDatasets = allDatasets.filter((dataset) => {
    if (!channelOptions.length) return true;
    return selectedChannels.includes(String(dataset.label));
  });

  const visibleDatasetLabelsKey = visibleDatasets
    .map((dataset) => dataset.label)
    .join("|");

  useEffect(() => {
    setChartZoomState((prev) => {
      const visibleLabels = new Set(visibleDatasets.map((dataset) => dataset.label));
      let changed = false;
      const next: Record<string, boolean> = {};

      Object.entries(prev).forEach(([label, zoomed]) => {
        if (visibleLabels.has(label)) {
          next[label] = zoomed;
        } else {
          changed = true;
        }
      });

      return changed ? next : prev;
    });
  }, [visibleDatasetLabelsKey, visibleDatasets]);

  useEffect(() => {
    onZoomStateChange?.(Object.values(chartZoomState).some(Boolean));
  }, [chartZoomState, onZoomStateChange]);

  useEffect(() => {
    return () => {
      onZoomStateChange?.(false);
    };
  }, [onZoomStateChange]);

  const perChartHeight = visibleDatasets.length > 1 ? 250 : height;

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

  function getYAxisLabel(datasetLabel: string): string {
    if (!data.unit) return datasetLabel;
    return `${datasetLabel} (${data.unit})`;
  }

  function setDatasetZoomState(label: string, zoomed: boolean) {
    setChartZoomState((prev) => {
      if ((prev[label] ?? false) === zoomed) return prev;
      return { ...prev, [label]: zoomed };
    });
  }

  function handleResetZoom(label: string) {
    chartRefs.current[label]?.resetZoom();
    setDatasetZoomState(label, false);
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

      <div className="sp-plot-stack">
        {visibleDatasets.map((dataset) => {
          const chartData: ChartData<"line"> = {
            datasets: [dataset],
          };

          return (
            <section key={dataset.label} className="sp-plot-card">
              <div className="sp-plot-card-header">
                <div className="sp-plot-heading">
                  <div className="sp-plot-title-row">
                    <span className="sp-plot-title">{title}</span>
                    <span className="sp-plot-badge">{dataset.label}</span>
                  </div>

                  <div className="sp-plot-meta">
                    {dataset.data.length} samples
                  </div>
                </div>

                <div className="sp-plot-actions">
                  <button
                    type="button"
                    className="sp-plot-btn"
                    onClick={() => handleResetZoom(dataset.label)}
                  >
                    Reset zoom
                  </button>
                </div>
              </div>

              <div className="sp-plot-canvas" style={{ height: perChartHeight }}>
                <Line
                  ref={(chart) => {
                    chartRefs.current[dataset.label] = chart ?? null;
                  }}
                  data={chartData}
                  options={buildChartOptions(
                    data.points,
                    getYAxisLabel(dataset.label),
                    (zoomed) => setDatasetZoomState(dataset.label, zoomed)
                  )}
                />
              </div>

              <div className="sp-plot-hint">
                Ctrl + wheel to zoom, Alt + drag to zoom box, Shift + drag to pan
              </div>
            </section>
          );
        })}
      </div>
    </div>
  );
}

export default memo(SensorLineChart);