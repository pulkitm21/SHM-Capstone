import { useEffect, useMemo, useState } from "react";
import {
  downloadFaultExport,
  downloadSensorExport,
  getFaults,
  getNodes,
  type FaultFilterOptions,
  type NodeRecord,
} from "../../services/api";
import "./Export.css";

type ExportMode = "range" | "full";

type FaultExportFilters = {
  start_day: string;
  end_day: string;
  serial_number: string;
  sensor_type: string;
  fault_type: string;
  severity: string;
  fault_status: string;
};

const EMPTY_OPTIONS: FaultFilterOptions = {
  sensor_types: [],
  fault_types: [],
  severities: [],
  statuses: [],
};

const HOUR_OPTIONS = Array.from({ length: 24 }, (_, index) =>
  String(index).padStart(2, "0")
);

function formatDateInput(date: Date) {
  return date.toISOString().slice(0, 10);
}

function getDefaultDateRange() {
  const end = new Date();
  const start = new Date();
  start.setDate(end.getDate() - 6);

  return {
    start_day: formatDateInput(start),
    end_day: formatDateInput(end),
  };
}

function toTitleCase(value: string) {
  return String(value || "")
    .replace(/[_-]+/g, " ")
    .replace(/\s+/g, " ")
    .trim()
    .replace(/\b\w/g, (char) => char.toUpperCase());
}

export default function ExportPage() {
  const defaultRange = useMemo(() => getDefaultDateRange(), []);

  const [mode, setMode] = useState<ExportMode>("range");
  const [faultFilters, setFaultFilters] = useState<FaultExportFilters>({
    start_day: defaultRange.start_day,
    end_day: defaultRange.end_day,
    serial_number: "",
    sensor_type: "",
    fault_type: "",
    severity: "",
    fault_status: "",
  });

  const [options, setOptions] = useState<FaultFilterOptions>(EMPTY_OPTIONS);
  const [loadingOptions, setLoadingOptions] = useState(true);

  const [nodes, setNodes] = useState<NodeRecord[]>([]);
  const [loadingNodes, setLoadingNodes] = useState(true);
  const [selectedNodeIds, setSelectedNodeIds] = useState<number[]>([]);
  const [sensorStartDay, setSensorStartDay] = useState(defaultRange.start_day);
  const [sensorEndDay, setSensorEndDay] = useState(defaultRange.end_day);
  const [sensorStartHour, setSensorStartHour] = useState("");
  const [sensorEndHour, setSensorEndHour] = useState("");
  const [includeRawData, setIncludeRawData] = useState(false);

  const [faultExporting, setFaultExporting] = useState(false);
  const [sensorExporting, setSensorExporting] = useState(false);

  const [error, setError] = useState("");
  const [message, setMessage] = useState("");

  useEffect(() => {
    const controller = new AbortController();

    async function loadFilterOptions() {
      try {
        setLoadingOptions(true);
        setError("");

        const response = await getFaults(
          {
            serial_number: faultFilters.serial_number || undefined,
            page: 1,
            page_size: 1,
          },
          controller.signal
        );

        if (controller.signal.aborted) return;

        setOptions(response.filter_options ?? EMPTY_OPTIONS);
        setLoadingOptions(false);
      } catch (err: any) {
        if (controller.signal.aborted) return;

        setOptions(EMPTY_OPTIONS);
        setLoadingOptions(false);
        setError(err?.message ?? "Failed to load fault export filters.");
      }
    }

    void loadFilterOptions();
    return () => controller.abort();
  }, [faultFilters.serial_number]);

  useEffect(() => {
    const controller = new AbortController();

    async function loadNodes() {
      try {
        setLoadingNodes(true);

        const response = await getNodes(controller.signal);
        if (controller.signal.aborted) return;

        const nodeRows = response.nodes ?? [];
        setNodes(nodeRows);
        setLoadingNodes(false);
      } catch (err: any) {
        if (controller.signal.aborted) return;

        setNodes([]);
        setLoadingNodes(false);
        setError(err?.message ?? "Failed to load nodes for sensor export.");
      }
    }

    void loadNodes();
    return () => controller.abort();
  }, []);

  function updateFaultFilter<K extends keyof FaultExportFilters>(
    key: K,
    value: FaultExportFilters[K]
  ) {
    setFaultFilters((current) => ({
      ...current,
      [key]: value,
    }));
  }

  function resetFaultFilters() {
    setFaultFilters({
      start_day: defaultRange.start_day,
      end_day: defaultRange.end_day,
      serial_number: "",
      sensor_type: "",
      fault_type: "",
      severity: "",
      fault_status: "",
    });
    setMode("range");
  }

  function resetSensorFilters() {
    setSensorStartDay(defaultRange.start_day);
    setSensorEndDay(defaultRange.end_day);
    setSensorStartHour("");
    setSensorEndHour("");
    setIncludeRawData(false);
    setSelectedNodeIds([]);
  }

  function toggleNodeSelection(nodeId: number) {
    setSelectedNodeIds((current) =>
      current.includes(nodeId)
        ? current.filter((id) => id !== nodeId)
        : [...current, nodeId].sort((a, b) => a - b)
    );
  }

  function selectAllNodes() {
    setSelectedNodeIds(nodes.map((node) => node.node_id));
  }

  function clearNodeSelection() {
    setSelectedNodeIds([]);
  }

  const faultRangeIsValid =
    !faultFilters.start_day ||
    !faultFilters.end_day ||
    faultFilters.start_day <= faultFilters.end_day;

  const sensorDateRangeIsValid =
    !!sensorStartDay && !!sensorEndDay && sensorStartDay <= sensorEndDay;

  const sensorHoursArePaired =
    (sensorStartHour && sensorEndHour) || (!sensorStartHour && !sensorEndHour);

  const sensorHourRangeIsValid =
    !sensorStartHour ||
    !sensorEndHour ||
    sensorStartDay < sensorEndDay ||
    sensorStartHour <= sensorEndHour;

  const sensorRangeIsValid =
    sensorDateRangeIsValid && !!sensorHoursArePaired && !!sensorHourRangeIsValid;

  async function handleFaultExport() {
    setFaultExporting(true);
    setError("");
    setMessage("");

    try {
      await downloadFaultExport({
        start_day: mode === "range" ? faultFilters.start_day : undefined,
        end_day: mode === "range" ? faultFilters.end_day : undefined,
        serial_number: faultFilters.serial_number || undefined,
        sensor_type: faultFilters.sensor_type || undefined,
        fault_type: faultFilters.fault_type || undefined,
        severity: faultFilters.severity ? Number(faultFilters.severity) : undefined,
        fault_status: faultFilters.fault_status || undefined,
      });

      setMessage("Fault log CSV download started.");
    } catch (err: any) {
      setError(err?.message ?? "Fault export failed.");
    } finally {
      setFaultExporting(false);
    }
  }

  async function handleSensorExport() {
    setSensorExporting(true);
    setError("");
    setMessage("");

    try {
      await downloadSensorExport({
        node_ids: selectedNodeIds,
        start_day: sensorStartDay,
        end_day: sensorEndDay,
        start_hour: sensorStartHour || undefined,
        end_hour: sensorEndHour || undefined,
        include_raw_data: includeRawData,
      });

      setMessage("Sensor data ZIP download started.");
    } catch (err: any) {
      setError(err?.message ?? "Sensor export failed.");
    } finally {
      setSensorExporting(false);
    }
  }

  return (
    <div className="export-page">
      <section className="export-hero">
        <div>
          <h1 className="export-title">Exports</h1>
        </div>
      </section>

      <section className="export-card">
        <div className="export-card-header">
          <div>
            <h2>Sensor Data Export</h2>
          </div>

          <div className="export-card-badge">ZIP</div>
        </div>

        <div className="export-grid export-grid-sensor">
          <div className="export-field">
            <label htmlFor="sensor-export-start-day">Start Day</label>
            <input
              id="sensor-export-start-day"
              type="date"
              value={sensorStartDay}
              onChange={(e) => setSensorStartDay(e.target.value)}
            />
          </div>

          <div className="export-field">
            <label htmlFor="sensor-export-start-hour">Start Hour</label>
            <select
              id="sensor-export-start-hour"
              value={sensorStartHour}
              onChange={(e) => setSensorStartHour(e.target.value)}
            >
              <option value="">Whole day</option>
              {HOUR_OPTIONS.map((hour) => (
                <option key={hour} value={hour}>
                  {hour}:00
                </option>
              ))}
            </select>
          </div>

          <div className="export-field">
            <label htmlFor="sensor-export-end-day">End Day</label>
            <input
              id="sensor-export-end-day"
              type="date"
              value={sensorEndDay}
              onChange={(e) => setSensorEndDay(e.target.value)}
            />
          </div>

          <div className="export-field">
            <label htmlFor="sensor-export-end-hour">End Hour</label>
            <select
              id="sensor-export-end-hour"
              value={sensorEndHour}
              onChange={(e) => setSensorEndHour(e.target.value)}
            >
              <option value="">Whole day</option>
              {HOUR_OPTIONS.map((hour) => (
                <option key={hour} value={hour}>
                  {hour}:00
                </option>
              ))}
            </select>
          </div>

          <div className="export-field export-field-wide export-option-field">
            <label htmlFor="sensor-export-include-raw">Export Options</label>

            {/* Toggle whether matching raw files are included in the ZIP. */}
            <label
              className="export-option-toggle"
              htmlFor="sensor-export-include-raw"
            >
              <input
                id="sensor-export-include-raw"
                type="checkbox"
                checked={includeRawData}
                onChange={(e) => setIncludeRawData(e.target.checked)}
              />
              <span className="export-option-copy">Include raw files</span>
            </label>
          </div>

          <div className="export-field export-field-wide export-node-field">
            <label>Nodes</label>

            <div className="export-node-toolbar">
              <button
                type="button"
                className="export-secondary-btn export-inline-btn"
                onClick={selectAllNodes}
                disabled={loadingNodes || nodes.length === 0}
              >
                Select All
              </button>

              <button
                type="button"
                className="export-secondary-btn export-inline-btn"
                onClick={clearNodeSelection}
                disabled={selectedNodeIds.length === 0}
              >
                Clear
              </button>

              <span className="export-node-count">
                {selectedNodeIds.length} selected
              </span>
            </div>

            <div className="export-node-list">
              {loadingNodes ? (
                <div className="export-node-empty">Loading nodes…</div>
              ) : nodes.length === 0 ? (
                <div className="export-node-empty">No nodes available.</div>
              ) : (
                nodes.map((node) => {
                  const checked = selectedNodeIds.includes(node.node_id);
                  return (
                    <label
                      key={node.node_id}
                      className={`export-node-option ${checked ? "selected" : ""}`}
                    >
                      <input
                        type="checkbox"
                        checked={checked}
                        onChange={() => toggleNodeSelection(node.node_id)}
                      />
                      <span className="export-node-option-main">
                        <span className="export-node-option-title">
                          {node.label || `Node ${node.node_id}`}
                        </span>
                        <span className="export-node-option-meta">
                          {node.serial}
                        </span>
                      </span>
                    </label>
                  );
                })
              )}
            </div>
          </div>
        </div>

        {!sensorDateRangeIsValid && (
          <div className="export-alert error">
            End day cannot be earlier than start day.
          </div>
        )}

        {!sensorHoursArePaired && (
          <div className="export-alert error">
            Start hour and end hour must both be provided, or both left as whole day.
          </div>
        )}

        {sensorHoursArePaired && !sensorHourRangeIsValid && (
          <div className="export-alert error">
            End hour cannot be earlier than start hour on the same day.
          </div>
        )}

        {error && <div className="export-alert error">{error}</div>}
        {message && <div className="export-alert success">{message}</div>}

        <div className="export-actions">
          <button
            type="button"
            className="export-secondary-btn"
            onClick={resetSensorFilters}
            disabled={sensorExporting}
          >
            Reset
          </button>

          <button
            type="button"
            className="export-primary-btn"
            onClick={handleSensorExport}
            disabled={
              sensorExporting ||
              !sensorRangeIsValid ||
              selectedNodeIds.length === 0
            }
          >
            {sensorExporting ? "Exporting..." : "Export ZIP"}
          </button>
        </div>
      </section>

      <section className="export-card">
        <div className="export-card-header">
          <div>
            <h2>Fault Log Export</h2>
          </div>

          <div className="export-card-badge">CSV</div>
        </div>

        <div className="export-mode-row">
          <button
            type="button"
            className={`export-mode-btn ${mode === "range" ? "active" : ""}`}
            onClick={() => setMode("range")}
          >
            Date Range
          </button>

          <button
            type="button"
            className={`export-mode-btn ${mode === "full" ? "active" : ""}`}
            onClick={() => setMode("full")}
          >
            Full Log
          </button>
        </div>

        <div className="export-grid">
          <div className="export-field">
            <label htmlFor="fault-export-start-day">Start Day</label>
            <input
              id="fault-export-start-day"
              type="date"
              value={faultFilters.start_day}
              onChange={(e) => updateFaultFilter("start_day", e.target.value)}
              disabled={mode === "full"}
            />
          </div>

          <div className="export-field">
            <label htmlFor="fault-export-end-day">End Day</label>
            <input
              id="fault-export-end-day"
              type="date"
              value={faultFilters.end_day}
              onChange={(e) => updateFaultFilter("end_day", e.target.value)}
              disabled={mode === "full"}
            />
          </div>

          <div className="export-field">
            <label htmlFor="fault-export-serial">Serial Number</label>
            <input
              id="fault-export-serial"
              type="text"
              value={faultFilters.serial_number}
              onChange={(e) => updateFaultFilter("serial_number", e.target.value)}
              placeholder="Search serial number"
            />
          </div>

          <div className="export-field">
            <label htmlFor="fault-export-sensor">Sensor</label>
            <select
              id="fault-export-sensor"
              value={faultFilters.sensor_type}
              onChange={(e) => updateFaultFilter("sensor_type", e.target.value)}
              disabled={loadingOptions}
            >
              <option value="">All sensors</option>
              {options.sensor_types.map((option) => (
                <option key={option} value={option}>
                  {toTitleCase(option)}
                </option>
              ))}
            </select>
          </div>

          <div className="export-field">
            <label htmlFor="fault-export-type">Fault Type</label>
            <select
              id="fault-export-type"
              value={faultFilters.fault_type}
              onChange={(e) => updateFaultFilter("fault_type", e.target.value)}
              disabled={loadingOptions}
            >
              <option value="">All fault types</option>
              {options.fault_types.map((option) => (
                <option key={option} value={option}>
                  {toTitleCase(option)}
                </option>
              ))}
            </select>
          </div>

          <div className="export-field">
            <label htmlFor="fault-export-severity">Severity</label>
            <select
              id="fault-export-severity"
              value={faultFilters.severity}
              onChange={(e) => updateFaultFilter("severity", e.target.value)}
              disabled={loadingOptions}
            >
              <option value="">All severities</option>
              {options.severities.map((option) => (
                <option key={option} value={String(option)}>
                  Severity {option}
                </option>
              ))}
            </select>
          </div>

          <div className="export-field">
            <label htmlFor="fault-export-status">Status</label>
            <select
              id="fault-export-status"
              value={faultFilters.fault_status}
              onChange={(e) => updateFaultFilter("fault_status", e.target.value)}
              disabled={loadingOptions}
            >
              <option value="">All statuses</option>
              {options.statuses.map((option) => (
                <option key={option} value={option}>
                  {toTitleCase(option)}
                </option>
              ))}
            </select>
          </div>
        </div>

        {!faultRangeIsValid && mode === "range" && (
          <div className="export-alert error">
            End day cannot be earlier than start day.
          </div>
        )}

        <div className="export-actions">
          <button
            type="button"
            className="export-secondary-btn"
            onClick={resetFaultFilters}
            disabled={faultExporting}
          >
            Reset
          </button>

          <button
            type="button"
            className="export-primary-btn"
            onClick={handleFaultExport}
            disabled={faultExporting || (mode === "range" && !faultRangeIsValid)}
          >
            {faultExporting ? "Exporting..." : "Export CSV"}
          </button>
        </div>
      </section>
    </div>
  );
}