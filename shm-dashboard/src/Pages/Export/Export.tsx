import { useEffect, useMemo, useState } from "react";
import {
  downloadFaultExport,
  getFaults,
  type FaultFilterOptions,
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
  description: string;
};

const EMPTY_OPTIONS: FaultFilterOptions = {
  sensor_types: [],
  fault_types: [],
  severities: [],
  statuses: [],
};

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
  const [filters, setFilters] = useState<FaultExportFilters>({
    start_day: defaultRange.start_day,
    end_day: defaultRange.end_day,
    serial_number: "",
    sensor_type: "",
    fault_type: "",
    severity: "",
    fault_status: "",
    description: "",
  });

  const [options, setOptions] = useState<FaultFilterOptions>(EMPTY_OPTIONS);
  const [loadingOptions, setLoadingOptions] = useState(true);
  const [exporting, setExporting] = useState(false);
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
            serial_number: filters.serial_number || undefined,
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
        setError(err?.message ?? "Failed to load export filters.");
      }
    }

    void loadFilterOptions();
    return () => controller.abort();
  }, [filters.serial_number]);

  function updateFilter<K extends keyof FaultExportFilters>(
    key: K,
    value: FaultExportFilters[K]
  ) {
    setFilters((current) => ({
      ...current,
      [key]: value,
    }));
  }

  function resetFilters() {
    setFilters({
      start_day: defaultRange.start_day,
      end_day: defaultRange.end_day,
      serial_number: "",
      sensor_type: "",
      fault_type: "",
      severity: "",
      fault_status: "",
      description: "",
    });
    setMode("range");
    setError("");
    setMessage("");
  }

  const rangeIsValid =
    !filters.start_day ||
    !filters.end_day ||
    filters.start_day <= filters.end_day;

  async function handleExport() {
    setExporting(true);
    setError("");
    setMessage("");

    try {
      await downloadFaultExport({
        start_day: mode === "range" ? filters.start_day : undefined,
        end_day: mode === "range" ? filters.end_day : undefined,
        serial_number: filters.serial_number || undefined,
        sensor_type: filters.sensor_type || undefined,
        fault_type: filters.fault_type || undefined,
        severity: filters.severity ? Number(filters.severity) : undefined,
        fault_status: filters.fault_status || undefined,
        description: filters.description || undefined,
      });

      setMessage("Fault log export started successfully.");
    } catch (err: any) {
      setError(err?.message ?? "Fault export failed.");
    } finally {
      setExporting(false);
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
              value={filters.start_day}
              onChange={(e) => updateFilter("start_day", e.target.value)}
              disabled={mode === "full"}
            />
          </div>

          <div className="export-field">
            <label htmlFor="fault-export-end-day">End Day</label>
            <input
              id="fault-export-end-day"
              type="date"
              value={filters.end_day}
              onChange={(e) => updateFilter("end_day", e.target.value)}
              disabled={mode === "full"}
            />
          </div>

          <div className="export-field">
            <label htmlFor="fault-export-serial">Serial Number</label>
            <input
              id="fault-export-serial"
              type="text"
              value={filters.serial_number}
              onChange={(e) => updateFilter("serial_number", e.target.value)}
              placeholder="Search serial number"
            />
          </div>

          <div className="export-field">
            <label htmlFor="fault-export-sensor">Sensor</label>
            <select
              id="fault-export-sensor"
              value={filters.sensor_type}
              onChange={(e) => updateFilter("sensor_type", e.target.value)}
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
              value={filters.fault_type}
              onChange={(e) => updateFilter("fault_type", e.target.value)}
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
              value={filters.severity}
              onChange={(e) => updateFilter("severity", e.target.value)}
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
              value={filters.fault_status}
              onChange={(e) => updateFilter("fault_status", e.target.value)}
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

          <div className="export-field export-field-wide">
            <label htmlFor="fault-export-description">Description</label>
            <input
              id="fault-export-description"
              type="text"
              value={filters.description}
              onChange={(e) => updateFilter("description", e.target.value)}
              placeholder="Search description"
            />
          </div>
        </div>

        {!rangeIsValid && mode === "range" && (
          <div className="export-alert error">
            End day cannot be earlier than start day.
          </div>
        )}

        {error && <div className="export-alert error">{error}</div>}
        {message && <div className="export-alert success">{message}</div>}

        <div className="export-actions">
          <button
            type="button"
            className="export-secondary-btn"
            onClick={resetFilters}
            disabled={exporting}
          >
            Reset
          </button>

          <button
            type="button"
            className="export-primary-btn"
            onClick={handleExport}
            disabled={exporting || (mode === "range" && !rangeIsValid)}
          >
            {exporting ? "Exporting..." : "Export CSV"}
          </button>
        </div>
      </section>
    </div>
  );
}