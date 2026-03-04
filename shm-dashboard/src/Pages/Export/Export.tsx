import { useEffect, useState } from "react";
import { getHealth, type HealthResponse } from "../../services/api";


export default function ExportPage() {
  // Track loading state
  const [loading, setLoading] = useState(true);

  // Store backend response
  const [result, setResult] = useState<HealthResponse | null>(null);

  // Store error messages
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {

    /**
     * AbortController cancels the fetch request
     * if the component unmounts before it completes.
     */
    const controller = new AbortController();

    async function run() {
      try {
        setLoading(true);
        setError(null);

        /**
         * Pass controller.signal into getHealth.
         * If controller.abort() is called,
         * the fetch request will immediately stop.
         */
        const json = await getHealth(controller.signal);

        setResult(json);
      } catch (e: any) {
        /**
         * If the request was aborted, ignore the error
         */
        if (controller.signal.aborted) return;

        setError(e?.message ?? String(e));
      } finally {
        if (!controller.signal.aborted) {
          setLoading(false);
        }
      }
    }

    run();

    /**
     * Cleanup function:
     * Runs when component unmounts.
     *
     * This cancels the in-flight request.
     */
    return () => {
      controller.abort();
    };
  }, []);

  return (
    <div style={{ padding: 18 }}>
      <h2 style={{ fontWeight: 800 }}>Export</h2>

        {/* Loading state */}
        {loading && <p>Testing connection…</p>}

        {/* Successful connection */}
        {!loading && result && (
          <div style={{ marginTop: 10 }}>
            <p>
              ✅ Connected to backend: <b>{result.status ?? "ok"}</b>
            </p>
            {result.time && <p>Time: {result.time}</p>}
          </div>
        )}

        {/* Error state */}
        {!loading && error && (
          <div style={{ marginTop: 10 }}>
            <p style={{ color: "crimson", fontWeight: 700 }}>
              ❌ Connection failed
            </p>
            <pre
              style={{
                background: "#f5f5f5",
                padding: 12,
                borderRadius: 8,
              }}
            >
              {error}
            </pre>
          </div>
        )}
      </div>
  );
}