// Log.tsx
import { useEffect, useMemo, useState } from "react";
import { getFaults, type FaultRow } from "../../services/api";
import LogRecent from "./LogRecent";
import LogNode from "./LogNode";
import LogTable from "./LogTable";
import "./Log.css";

export type FaultLogVariant = "recent" | "node" | "full";

type FaultLogProps = {
  serial_number?: string;
  limit?: number;
  variant?: FaultLogVariant;

  previewMode?: boolean;
  previewFaults?: FaultRow[];
};

type FaultEventsResponse = {
  faults?: FaultRow[];
};

// Sort faults newest-first so all UI variants display a consistent order.
function normalizeFaultRows(rows: FaultRow[] | undefined): FaultRow[] {
  if (!rows) return [];

  return [...rows].sort((a, b) => {
    const aTime = new Date(a.ts ?? 0).getTime();
    const bTime = new Date(b.ts ?? 0).getTime();
    return bTime - aTime;
  });
}

// Keep only active faults for summary-style variants.
function getActiveFaultRows(rows: FaultRow[]): FaultRow[] {
  return rows.filter(
    (fault) => String(fault.fault_status ?? "").toLowerCase() === "active"
  );
}

export default function FaultLog({
  serial_number,
  limit = 10,
  variant = "full",
  previewMode = false,
  previewFaults,
}: FaultLogProps) {
  // Guard against an undefined preview array.
  const safePreviewFaults = previewFaults ?? [];

  // Local state is only used for the non-table variants.
  const [faults, setFaults] = useState<FaultRow[]>([]);
  const [loading, setLoading] = useState(variant !== "full");
  const [error, setError] = useState("");

  useEffect(() => {
    // The full table manages its own fetching and state internally.
    if (variant === "full") {
      return;
    }

    // In preview mode, skip API calls and use the provided mock data.
    if (previewMode) {
      setFaults(normalizeFaultRows(safePreviewFaults.slice(0, limit)));
      setLoading(false);
      setError("");
      return;
    }

    let mounted = true;
    let eventSource: EventSource | null = null;
    let reconnectTimeoutId: number | null = null;

    // Fallback request used when SSE is unavailable or reconnecting.
    async function loadFaultsFallback() {
      try {
        setLoading(true);
        setError("");

        const response = await getFaults({
          serial_number,
          limit,
        });

        if (!mounted) return;

        setFaults(normalizeFaultRows(response.faults));
        setLoading(false);
        setError("");
      } catch (err: any) {
        if (!mounted) return;

        console.error(err);
        setFaults([]);
        setLoading(false);
        setError(err?.message ?? "Failed to load faults.");
      }
    }

    // Open an SSE connection for live fault updates.
    function connectFaultLogSSE() {
      const params = new URLSearchParams();
      if (serial_number) params.set("serial_number", serial_number);
      params.set("limit", String(limit));

      const query = params.toString();

      eventSource = new EventSource(
        `${import.meta.env.VITE_API_BASE_URL}/api/events/faults${
          query ? `?${query}` : ""
        }`
      );

      eventSource.onopen = () => {
        if (!mounted) return;

        // Keep the loading state on until the first message arrives.
        setLoading(true);
        setError("");
      };

      eventSource.onmessage = (event) => {
        if (!mounted) return;

        try {
          const data: FaultEventsResponse = JSON.parse(event.data);
          setFaults(normalizeFaultRows(data.faults));
          setLoading(false);
          setError("");
        } catch (err) {
          console.error(err);
          setLoading(false);
          setError("Failed to parse live fault updates.");
        }
      };

      eventSource.onerror = () => {
        if (!mounted) return;

        eventSource?.close();
        setLoading(false);
        setError("Fault log connection lost — retrying...");

        // Try a REST refresh first, then reopen SSE after a short delay.
        reconnectTimeoutId = window.setTimeout(() => {
          if (!mounted) return;
          void loadFaultsFallback();
          connectFaultLogSSE();
        }, 3000);
      };
    }

    connectFaultLogSSE();

    return () => {
      mounted = false;
      eventSource?.close();

      if (reconnectTimeoutId !== null) {
        window.clearTimeout(reconnectTimeoutId);
      }
    };
  }, [serial_number, limit, previewMode, safePreviewFaults, variant]);

  // Only active faults are shown in the recent and node variants.
  const displayFaults = useMemo(() => {
    if (variant === "full") return faults;
    return getActiveFaultRows(faults);
  }, [faults, variant]);

  if (variant === "recent") {
    return <LogRecent faults={displayFaults} loading={loading} error={error} />;
  }

  if (variant === "node") {
    return <LogNode faults={displayFaults} loading={loading} error={error} />;
  }

  return <LogTable serial_number={serial_number} />;
}