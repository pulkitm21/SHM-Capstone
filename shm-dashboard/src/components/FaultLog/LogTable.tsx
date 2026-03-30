import { useEffect, useMemo, useState } from "react";

import {
  getFaults,
  type FaultFilterOptions,
  type FaultRow,
} from "../../services/api";

import {
  formatFaultTimestamp,
  getSeverityClass,
  getStatusClass,
  toTitleCase,
} from "./faultLog.utils";

type Props = {
  serial_number?: string;
};

type FaultFilters = {
  serial_number: string;
  sensor_type: string;
  fault_type: string;
  severity: string;
  fault_status: string;
  start_date: string;
  end_date: string;
  description: string;
};

// Number of rows to request from the backend per table page.
const PAGE_SIZE = 12;

// Default empty state for all filter inputs.
const DEFAULT_FILTERS: FaultFilters = {
  serial_number: "",
  sensor_type: "",
  fault_type: "",
  severity: "",
  fault_status: "",
  start_date: "",
  end_date: "",
  description: "",
};

// Default empty dropdown options until the first API response arrives.
const EMPTY_OPTIONS: FaultFilterOptions = {
  sensor_types: [],
  fault_types: [],
  severities: [],
  statuses: [],
};

export default function LogTable({ serial_number }: Props) {
  // Store the current filter values shown in the filter bar.
  const [filters, setFilters] = useState<FaultFilters>({
    ...DEFAULT_FILTERS,
    serial_number: serial_number ?? "",
  });

  // Store the current page of results and server-provided metadata.
  const [faults, setFaults] = useState<FaultRow[]>([]);
  const [options, setOptions] = useState<FaultFilterOptions>(EMPTY_OPTIONS);
  const [page, setPage] = useState(1);
  const [totalItems, setTotalItems] = useState(0);
  const [totalPages, setTotalPages] = useState(1);

  // Track async request state for loading and error feedback.
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");

  // Used to force a manual reload without changing filters or page state.
  const [refreshTick, setRefreshTick] = useState(0);

  // Show the last successful fetch time in the UI.
  const [lastUpdated, setLastUpdated] = useState<Date | null>(null);

  // Keep the serial filter in sync when the parent changes the selected node.
  useEffect(() => {
    setFilters((current) => ({
      ...current,
      serial_number: serial_number ?? "",
    }));
    setPage(1);
  }, [serial_number]);

  // Fetch the current page whenever filters, page, or manual refresh changes.
  useEffect(() => {
    const controller = new AbortController();

    async function loadFaultPage() {
      try {
        setLoading(true);
        setError("");

        const response = await getFaults(
          {
            serial_number: filters.serial_number || undefined,
            sensor_type: filters.sensor_type || undefined,
            fault_type: filters.fault_type || undefined,
            severity: filters.severity ? Number(filters.severity) : undefined,
            fault_status: filters.fault_status || undefined,
            description: filters.description || undefined,
            start_date: filters.start_date || undefined,
            end_date: filters.end_date || undefined,
            page,
            page_size: PAGE_SIZE,
          },
          controller.signal
        );

        setFaults(response.faults ?? []);
        setOptions(response.filter_options ?? EMPTY_OPTIONS);
        setTotalItems(response.total_items ?? 0);
        setTotalPages(Math.max(1, response.total_pages ?? 1));
        setLastUpdated(new Date());
        setLoading(false);
      } catch (err: any) {
        if (controller.signal.aborted) return;

        console.error(err);
        setFaults([]);
        setOptions(EMPTY_OPTIONS);
        setTotalItems(0);
        setTotalPages(1);
        setLoading(false);
        setError(err?.message ?? "Failed to load faults.");
      }
    }

    void loadFaultPage();

    return () => controller.abort();
  }, [filters, page, refreshTick]);

  // Check whether any non-default filters are active to enable reset behavior.
  const hasActiveFilters = useMemo(() => {
    return Object.entries(filters).some(([key, value]) => {
      if (key === "serial_number" && serial_number) {
        return value.trim() !== serial_number.trim();
      }
      return value.trim() !== "";
    });
  }, [filters, serial_number]);

  // Update one filter field and jump back to the first page of results.
  function updateFilter<K extends keyof FaultFilters>(
    key: K,
    value: FaultFilters[K]
  ) {
    setFilters((current) => ({
      ...current,
      [key]: value,
    }));
    setPage(1);
  }

  // Reset filters while preserving a parent-provided serial number when present.
  function clearFilters() {
    setFilters({
      ...DEFAULT_FILTERS,
      serial_number: serial_number ?? "",
    });
    setPage(1);
  }

  return (
    <section className="faultlog-panel faultlog-table-panel">
      <div className="faultlog-panel-top">
        <div className="faultlog-panel-summary">
          {/* Summary pill for total matching records across all pages. */}
          <span className="faultlog-summary-chip">Total: {totalItems}</span>

          <div className="faultlog-panel-actions">
            {/* Last successful refresh time for manual polling visibility. */}
            <span className="faultlog-last-updated">
              {lastUpdated
                ? `Updated ${lastUpdated.toLocaleTimeString()}`
                : "Not updated yet"}
            </span>

            {/* Manual reload button for cases where SSE is not used on the table. */}
            <button
              type="button"
              className="faultlog-refresh-btn"
              onClick={() => setRefreshTick((v) => v + 1)}
              disabled={loading}
            >
              Refresh
            </button>
          </div>
        </div>
      </div>

      <div className="faultlog-filter-bar">
        <div className="faultlog-filter-grid">
          <div className="faultlog-filter-field">
            <label htmlFor="fault-filter-serial">Serial Number</label>
            <input
              id="fault-filter-serial"
              type="text"
              value={filters.serial_number}
              onChange={(e) => updateFilter("serial_number", e.target.value)}
              placeholder="Search serial number"
            />
          </div>

          <div className="faultlog-filter-field">
            <label htmlFor="fault-filter-sensor">Sensor</label>
            <select
              id="fault-filter-sensor"
              value={filters.sensor_type}
              onChange={(e) => updateFilter("sensor_type", e.target.value)}
            >
              <option value="">All sensors</option>
              {options.sensor_types.map((option) => (
                <option key={option} value={option}>
                  {toTitleCase(option)}
                </option>
              ))}
            </select>
          </div>

          <div className="faultlog-filter-field">
            <label htmlFor="fault-filter-type">Fault Type</label>
            <select
              id="fault-filter-type"
              value={filters.fault_type}
              onChange={(e) => updateFilter("fault_type", e.target.value)}
            >
              <option value="">All fault types</option>
              {options.fault_types.map((option) => (
                <option key={option} value={option}>
                  {toTitleCase(option)}
                </option>
              ))}
            </select>
          </div>

          <div className="faultlog-filter-field">
            <label htmlFor="fault-filter-severity">Severity</label>
            <select
              id="fault-filter-severity"
              value={filters.severity}
              onChange={(e) => updateFilter("severity", e.target.value)}
            >
              <option value="">All severities</option>
              {options.severities.map((option) => (
                <option key={option} value={String(option)}>
                  Severity {option}
                </option>
              ))}
            </select>
          </div>

          <div className="faultlog-filter-field">
            <label htmlFor="fault-filter-status">Status</label>
            <select
              id="fault-filter-status"
              value={filters.fault_status}
              onChange={(e) => updateFilter("fault_status", e.target.value)}
            >
              <option value="">All statuses</option>
              {options.statuses.map((option) => (
                <option key={option} value={option}>
                  {toTitleCase(option)}
                </option>
              ))}
            </select>
          </div>

          <div className="faultlog-filter-field">
            <label htmlFor="fault-filter-start-date">From Date</label>
            <input
              id="fault-filter-start-date"
              type="date"
              value={filters.start_date}
              onChange={(e) => updateFilter("start_date", e.target.value)}
            />
          </div>

          <div className="faultlog-filter-field">
            <label htmlFor="fault-filter-end-date">To Date</label>
            <input
              id="fault-filter-end-date"
              type="date"
              value={filters.end_date}
              onChange={(e) => updateFilter("end_date", e.target.value)}
            />
          </div>

          <div className="faultlog-filter-field faultlog-filter-field-wide">
            <label htmlFor="fault-filter-description">Description</label>
            <input
              id="fault-filter-description"
              type="text"
              value={filters.description}
              onChange={(e) => updateFilter("description", e.target.value)}
              placeholder="Search description"
            />
          </div>

          <div className="faultlog-filter-field faultlog-filter-field-button">
            {/* Hidden spacer keeps the button aligned with the rest of the grid. */}
            <label className="faultlog-filter-spacer" aria-hidden="true">
              Actions
            </label>
            <button
              type="button"
              className="faultlog-clear-btn"
              onClick={clearFilters}
              disabled={!hasActiveFilters}
            >
              Remove Filters
            </button>
          </div>
        </div>
      </div>

      {loading ? (
        <div className="faultlog-empty">Loading faults…</div>
      ) : error ? (
        <div className="faultlog-empty">Unable to load faults: {error}</div>
      ) : faults.length === 0 ? (
        <div className="faultlog-empty">
          No faults match the current filter selection.
        </div>
      ) : (
        <>
          <div className="faultlog-table-wrap">
            <table className="faultlog-table">
              <thead>
                <tr>
                  <th>Timestamp</th>
                  <th>Serial Number</th>
                  <th>Sensor</th>
                  <th>Fault Type</th>
                  <th>Severity</th>
                  <th>Status</th>
                  <th>Description</th>
                </tr>
              </thead>

              <tbody>
                {faults.map((fault) => (
                  <tr key={`${fault.id}-${fault.ts}`}>
                    <td>{formatFaultTimestamp(fault.ts)}</td>
                    <td className="faultlog-cell-source">
                      {fault.serial_number || "Backend"}
                    </td>
                    <td>{toTitleCase(fault.sensor_type)}</td>
                    <td>{toTitleCase(fault.fault_type)}</td>
                    <td>
                      <span
                        className={`faultlog-pill severity ${getSeverityClass(
                          Number(fault.severity)
                        )}`}
                      >
                        {fault.severity ?? "—"}
                      </span>
                    </td>
                    <td>
                      <span
                        className={`faultlog-pill status ${getStatusClass(
                          fault.fault_status
                        )}`}
                      >
                        {toTitleCase(fault.fault_status)}
                      </span>
                    </td>
                    <td className="faultlog-description-cell">
                      {fault.description || "—"}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>

          <div className="faultlog-pagination">
            {/* Simple page indicator for the current table state. */}
            <div className="faultlog-pagination-info">
              Page {page} of {totalPages}
            </div>

            <div className="faultlog-pagination-actions">
              <button
                type="button"
                className="faultlog-page-btn"
                onClick={() => setPage((current) => Math.max(1, current - 1))}
                disabled={page <= 1}
              >
                Previous
              </button>

              <button
                type="button"
                className="faultlog-page-btn"
                onClick={() =>
                  setPage((current) => Math.min(totalPages, current + 1))
                }
                disabled={page >= totalPages}
              >
                Next
              </button>
            </div>
          </div>
        </>
      )}
    </section>
  );
}