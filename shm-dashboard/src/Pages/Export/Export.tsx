import { useEffect, useState } from "react";

const API_BASE = import.meta.env.VITE_API_BASE_URL;

type HealthResponse = {
  status: string;
  time?: string;
};

export default function ExportPage() {
  const [loading, setLoading] = useState(true);
  const [result, setResult] = useState<HealthResponse | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;

    async function run() {
      try {
        setLoading(true);
        setError(null);

        if (!API_BASE) {
          throw new Error("VITE_API_BASE_URL is missing. Check your .env and restart npm run dev.");
        }

        const res = await fetch(`${API_BASE}/health`);
        if (!res.ok) throw new Error(`Health check failed: ${res.status}`);

        const json = (await res.json()) as HealthResponse;
        if (cancelled) return;

        setResult(json);
      } catch (e: any) {
        if (cancelled) return;
        setError(e?.message ?? String(e));
      } finally {
        if (!cancelled) setLoading(false);
      }
    }

    run();
    return () => {
      cancelled = true;
    };
  }, []);

  return (
    <div style={{ padding: 18 }}>
      <h2 style={{ fontWeight: 800 }}>Export</h2>

      <div style={{ marginTop: 12 }}>
        <div style={{ fontFamily: "ui-monospace, SFMono-Regular, Menlo, monospace" }}>
          API_BASE: {API_BASE || "(not set)"}
        </div>

        {loading && <p>Testing connection…</p>}

        {!loading && result && (
          <div style={{ marginTop: 10 }}>
            <p>
              ✅ Connected to backend: <b>{result.status}</b>
            </p>
            {result.time && <p>Time: {result.time}</p>}
          </div>
        )}

        {!loading && error && (
          <div style={{ marginTop: 10 }}>
            <p style={{ color: "crimson", fontWeight: 700 }}>❌ Connection failed</p>
            <pre
              style={{
                background: "#f5f5f5",
                padding: 12,
                borderRadius: 8,
                overflowX: "auto",
              }}
            >
              {error}
            </pre>

            <p style={{ marginTop: 8 }}>
              Common fixes:
              <br />• Start backend with <code>--host 0.0.0.0</code>
              <br />• Check Pi IP + port 8000
              <br />• Add CORS allow for <code>http://localhost:5173</code>
            </p>
          </div>
        )}
      </div>
    </div>
  );
}
