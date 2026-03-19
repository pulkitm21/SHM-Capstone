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
  title?: string;
  previewMode?: boolean;
  previewFaults?: FaultRow[];
};

type FaultEventsResponse = {
  faults?: FaultRow[];
  time?: string;
};

function normalizeFaultRows(rows: FaultRow[] | undefined): FaultRow[] {
  if (!rows) return [];

  return [...rows].sort((a, b) => {
    const aTime = new Date(a.ts ?? 0).getTime();
    const bTime = new Date(b.ts ?? 0).getTime();
    return bTime - aTime;
  });
}

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
  previewFaults = [],
}: FaultLogProps) {
  const [faults, setFaults] = useState<FaultRow[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");

  useEffect(() => {
    if (previewMode) {
      setFaults(normalizeFaultRows(previewFaults.slice(0, limit)));
      setLoading(false);
      setError("");
      return;
    }

    let mounted = true;
    let eventSource: EventSource | null = null;
    let reconnectTimeoutId: number | null = null;

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
        console.error(err);
        if (!mounted) return;

        setFaults([]);
        setLoading(false);
        setError(err?.message ?? "Failed to load faults.");
      }
    }

    function connectFaultLogSSE() {
      const params = new URLSearchParams();
      if (serial_number) params.set("serial_number", serial_number);
      params.set("limit", String(limit));

      const query = params.toString();

      eventSource = new EventSource(
        `${import.meta.env.VITE_API_BASE_URL}/api/events/faults${query ? `?${query}` : ""}`
      );

      eventSource.onopen = () => {
        if (!mounted) return;
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
  }, [serial_number, limit, previewMode, previewFaults]);

  const displayFaults = useMemo(() => {
    if (variant === "full") return faults;
    return getActiveFaultRows(faults);
  }, [faults, variant]);


  if (variant === "recent") {
    return (
      <LogRecent
        faults={displayFaults}
        loading={loading}
        error={error}
      />
    );
  }

  if (variant === "node") {
    return (
      <LogNode
        faults={displayFaults}
        loading={loading}
        error={error}
      />
    );
  }

  return (
    <LogTable
      faults={displayFaults}
      loading={loading}
      error={error}
    />
  );
}