import { useMemo, useRef, useState } from "react";
import {
  collectRawInputEntries,
  decodeRawEntriesToCsv,
  EMPTY_PROGRESS,
  readMetadataPreviewFromZip,
  type DecodeBatchResult,
  type ProgressSnapshot,
  type RawInputEntry,
} from "./decoderUtils";
import "./Decoder.css";

type QueueSummary = {
  sourceFileCount: number;
  rawEntryCount: number;
  rejectedNames: string[];
  metadataPreview: string;
};

const ACCEPTED_FILE_TYPES = ".bin,.gz,.gzip,.zip";

function formatProgressLabel(progress: ProgressSnapshot) {
  if (progress.phase === "idle") return "Waiting for files";
  if (progress.phase === "reading") return `Reading file ${progress.completed + 1} of ${progress.total}`;
  if (progress.phase === "decoding") return `Decoding file ${progress.completed + 1} of ${progress.total}`;
  if (progress.phase === "packaging") return "Packaging decoded CSV files";
  return progress.currentLabel || "Done";
}

export default function DecoderPage() {
  const fileInputRef = useRef<HTMLInputElement | null>(null);

  const [queueEntries, setQueueEntries] = useState<RawInputEntry[]>([]);
  const [summary, setSummary] = useState<QueueSummary>({
    sourceFileCount: 0,
    rawEntryCount: 0,
    rejectedNames: [],
    metadataPreview: "",
  });
  const [processing, setProcessing] = useState(false);
  const [progress, setProgress] = useState<ProgressSnapshot>(EMPTY_PROGRESS);
  const [result, setResult] = useState<DecodeBatchResult | null>(null);
  const [error, setError] = useState("");

  const hasQueue = queueEntries.length > 0;
  const progressLabel = useMemo(() => formatProgressLabel(progress), [progress]);

  async function handleSelectedFiles(fileList: FileList | null) {
    if (!fileList || fileList.length === 0) return;

    const files = Array.from(fileList);
    setError("");
    setResult(null);
    setProgress(EMPTY_PROGRESS);

    try {
      const { entries, rejectedNames } = await collectRawInputEntries(files);
      const zipFile = files.find((file) => /\.zip$/i.test(file.name));
      const metadataPreview = zipFile ? await readMetadataPreviewFromZip(zipFile) : "";

      setQueueEntries(entries);
      setSummary({
        sourceFileCount: files.length,
        rawEntryCount: entries.length,
        rejectedNames,
        metadataPreview,
      });

      if (entries.length === 0) {
        setError("No supported .bin, .bin.gz, .gzip, or .zip files were found in the selected input.");
      }
    } catch (err) {
      setQueueEntries([]);
      setSummary({
        sourceFileCount: 0,
        rawEntryCount: 0,
        rejectedNames: [],
        metadataPreview: "",
      });
      setError(err instanceof Error ? err.message : "Failed to read selected files.");
    } finally {
      if (fileInputRef.current) {
        fileInputRef.current.value = "";
      }
    }
  }

  async function handleDecode() {
    if (!hasQueue) {
      setError("Add at least one supported raw file before decoding.");
      return;
    }

    setProcessing(true);
    setError("");
    setResult(null);

    try {
      const nextResult = await decodeRawEntriesToCsv(queueEntries, setProgress);
      setResult(nextResult);

      if (!nextResult.downloadedFileName && nextResult.failures.length > 0) {
        setError("No decoded CSV files were generated. Review the failure details below.");
      }
    } catch (err) {
      setError(err instanceof Error ? err.message : "Decoding failed.");
    } finally {
      setProcessing(false);
    }
  }

  function handleClear() {
    setQueueEntries([]);
    setSummary({
      sourceFileCount: 0,
      rawEntryCount: 0,
      rejectedNames: [],
      metadataPreview: "",
    });
    setResult(null);
    setProgress(EMPTY_PROGRESS);
    setError("");
    if (fileInputRef.current) {
      fileInputRef.current.value = "";
    }
  }

  return (
    <div className="decoder-page">
      <section className="decoder-hero">
        <div>
          <h1 className="decoder-title">Decoder</h1>
          <p className="decoder-subtitle">
            Decode raw sensor exports on the user device. This page accepts individual .bin files,
            .bin.gz files, or a raw export ZIP and converts supported inputs into per-sensor CSV
            outputs without using the Pi backend.
          </p>
        </div>
      </section>

      <section className="decoder-card">
        <div className="decoder-card-header">
          <div>
            <h2>Input Files</h2>
            <p>
              Add raw files individually or load an exported ZIP archive. The page expands ZIP
              contents locally in the browser and queues supported raw file members for decoding.
            </p>
          </div>
          <div className="decoder-card-badge">Local</div>
        </div>

        <div className="decoder-upload-panel">
          <input
            ref={fileInputRef}
            id="decoder-file-input"
            type="file"
            accept={ACCEPTED_FILE_TYPES}
            multiple
            className="decoder-file-input"
            onChange={(event) => void handleSelectedFiles(event.target.files)}
          />

          <label htmlFor="decoder-file-input" className="decoder-dropzone">
            <span className="decoder-dropzone-title">Choose raw files or an export ZIP</span>
            <span className="decoder-dropzone-copy">
              Supported input types: .bin, .bin.gz, .gzip, and .zip
            </span>
          </label>
        </div>

        <div className="decoder-summary-grid">
          <div className="decoder-stat-card">
            <span className="decoder-stat-label">Source files</span>
            <strong>{summary.sourceFileCount}</strong>
          </div>
          <div className="decoder-stat-card">
            <span className="decoder-stat-label">Queued raw files</span>
            <strong>{summary.rawEntryCount}</strong>
          </div>
          <div className="decoder-stat-card">
            <span className="decoder-stat-label">Rejected files</span>
            <strong>{summary.rejectedNames.length}</strong>
          </div>
        </div>

        {summary.rejectedNames.length > 0 && (
          <div className="decoder-alert warning">
            Ignored unsupported files: {summary.rejectedNames.join(", ")}
          </div>
        )}

        {summary.metadataPreview && (
          <div className="decoder-metadata-panel">
            <h3>Export Metadata Preview</h3>
            <pre>{summary.metadataPreview}</pre>
          </div>
        )}

        <div className="decoder-queue-list">
          {queueEntries.length === 0 ? (
            <div className="decoder-empty">No raw files queued yet.</div>
          ) : (
            queueEntries.map((entry) => (
              <div key={`${entry.sourceName}:${entry.entryName}`} className="decoder-queue-item">
                <div className="decoder-queue-main">
                  <span className="decoder-queue-name">{entry.entryName}</span>
                  <span className="decoder-queue-source">Source: {entry.sourceName}</span>
                </div>
              </div>
            ))
          )}
        </div>

        <div className="decoder-actions">
          <button
            type="button"
            className="decoder-secondary-btn"
            onClick={handleClear}
            disabled={processing && !hasQueue}
          >
            Clear
          </button>
          <button
            type="button"
            className="decoder-primary-btn"
            onClick={() => fileInputRef.current?.click()}
            disabled={processing}
          >
            Add Files
          </button>
          <button
            type="button"
            className="decoder-primary-btn"
            onClick={() => void handleDecode()}
            disabled={processing || !hasQueue}
          >
            {processing ? "Decoding..." : "Decode and Download"}
          </button>
        </div>
      </section>

      <section className="decoder-card">
        <div className="decoder-card-header">
          <div>
            <h2>Progress</h2>
            <p>
              Files are processed one at a time in the browser to keep memory usage more stable on
              the user device during large batches.
            </p>
          </div>
        </div>

        <div className="decoder-progress-shell" aria-live="polite">
          <div className="decoder-progress-track">
            <div className="decoder-progress-fill" style={{ width: `${progress.percent}%` }} />
          </div>
          <div className="decoder-progress-meta">
            <span>{progressLabel}</span>
            <span>{progress.percent}%</span>
          </div>
        </div>

        {error && <div className="decoder-alert error">{error}</div>}

        {result?.downloadedFileName && (
          <div className="decoder-alert success">
            Download started: {result.downloadedFileName}
          </div>
        )}

        {result && (
          <div className="decoder-results-grid">
            <div className="decoder-result-panel">
              <h3>Decoded Files</h3>
              {result.successes.length === 0 ? (
                <div className="decoder-empty">No successful decodes yet.</div>
              ) : (
                <div className="decoder-result-list">
                  {result.successes.map((success) => (
                    <div key={success.entryName} className="decoder-result-item">
                      <span className="decoder-result-name">{success.entryName}</span>
                      <span className="decoder-result-meta">
                        {success.outputFiles.length} CSV file(s)
                      </span>
                    </div>
                  ))}
                </div>
              )}
            </div>

            <div className="decoder-result-panel">
              <h3>Skipped or Failed Files</h3>
              {result.failures.length === 0 ? (
                <div className="decoder-empty">No failures.</div>
              ) : (
                <div className="decoder-result-list">
                  {result.failures.map((failure) => (
                    <div key={`${failure.entryName}:${failure.reason}`} className="decoder-result-item error">
                      <span className="decoder-result-name">{failure.entryName}</span>
                      <span className="decoder-result-meta">{failure.reason}</span>
                    </div>
                  ))}
                </div>
              )}
            </div>
          </div>
        )}
      </section>
    </div>
  );
}
