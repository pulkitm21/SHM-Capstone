import JSZip from "jszip";

export type RawInputEntry = {
  sourceName: string;
  entryName: string;
  displayName: string;
  loadBytes: () => Promise<ArrayBuffer>;
};

export type DecodedOutputFile = {
  fileName: string;
  content: string;
};

export type DecodeSuccess = {
  entryName: string;
  outputFiles: DecodedOutputFile[];
};

export type DecodeFailure = {
  entryName: string;
  reason: string;
};

export type DecodeBatchResult = {
  successes: DecodeSuccess[];
  failures: DecodeFailure[];
  downloadedFileName?: string;
};

export type ProgressSnapshot = {
  phase: "idle" | "reading" | "decoding" | "packaging" | "done";
  completed: number;
  total: number;
  percent: number;
  currentLabel: string;
};

export const EMPTY_PROGRESS: ProgressSnapshot = {
  phase: "idle",
  completed: 0,
  total: 0,
  percent: 0,
  currentLabel: "",
};

const RAW_FILE_PATTERN = /\.bin(?:\.gz|\.gzip)?$/i;
const ZIP_FILE_PATTERN = /\.zip$/i;

function isZipFileName(name: string) {
  return ZIP_FILE_PATTERN.test(name);
}

function isRawSensorFileName(name: string) {
  return RAW_FILE_PATTERN.test(name);
}

function normalizeRawOutputBaseName(name: string) {
  return name.replace(/\.gzip$/i, ".gz").replace(/\.bin\.gz$/i, "").replace(/\.bin$/i, "");
}

async function readTextFromBlob(blob: Blob) {
  return await blob.text();
}

function arrayBufferToBlob(buffer: ArrayBuffer, type = "application/octet-stream") {
  return new Blob([buffer], { type });
}

async function gunzipArrayBuffer(input: ArrayBuffer): Promise<ArrayBuffer> {
  if (typeof DecompressionStream === "undefined") {
    throw new Error("This browser does not support gzip decompression in the decoder page.");
  }

  const sourceBlob = arrayBufferToBlob(input);
  const decompressedStream = sourceBlob.stream().pipeThrough(new DecompressionStream("gzip"));
  const response = new Response(decompressedStream);
  return await response.arrayBuffer();
}

function triggerBlobDownload(blob: Blob, fileName: string) {
  const objectUrl = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = objectUrl;
  anchor.download = fileName;
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
  URL.revokeObjectURL(objectUrl);
}

async function buildDownloadArtifact(outputFiles: DecodedOutputFile[]) {
  if (outputFiles.length === 1) {
    const onlyFile = outputFiles[0];
    return {
      fileName: onlyFile.fileName,
      blob: new Blob([onlyFile.content], { type: "text/csv;charset=utf-8" }),
    };
  }

  const zip = new JSZip();
  for (const file of outputFiles) {
    zip.file(file.fileName, file.content);
  }

  const blob = await zip.generateAsync({ type: "blob" });
  return {
    fileName: `decoded_sensor_csv_${new Date().toISOString().slice(0, 10)}.zip`,
    blob,
  };
}

export async function collectRawInputEntries(files: File[]) {
  const entries: RawInputEntry[] = [];
  const rejectedNames: string[] = [];

  for (const file of files) {
    if (isZipFileName(file.name)) {
      const archive = await JSZip.loadAsync(file);
      archive.forEach((relativePath, entry) => {
        if (entry.dir) return;
        if (!isRawSensorFileName(relativePath)) return;

        entries.push({
          sourceName: file.name,
          entryName: relativePath,
          displayName: `${file.name} -> ${relativePath}`,
          loadBytes: async () => {
            const uint8 = await entry.async("uint8array");
            return uint8.slice().buffer;
          },
        });
      });
      continue;
    }

    if (isRawSensorFileName(file.name)) {
      entries.push({
        sourceName: file.name,
        entryName: file.name,
        displayName: file.name,
        loadBytes: async () => {
          const buffer = await file.arrayBuffer();
          return buffer.slice(0);
        },
      });
      continue;
    }

    rejectedNames.push(file.name);
  }

  return { entries, rejectedNames };
}

async function decodeThresholdEncodedBinaryToSensorCsvFiles(
  _bytes: ArrayBuffer,
  baseName: string
): Promise<DecodedOutputFile[]> {
  /*
    Placeholder for the new threshold-based decoder.
    Replace this function once the binary decode logic is finalized.
    The function should return one CSV string per sensor type for a single raw input file.
  */
  throw new Error(
    `Threshold-based decoder not implemented yet for '${baseName}'. Add the decode logic in decoderUtils.ts.`
  );
}

export async function decodeRawEntriesToCsv(
  entries: RawInputEntry[],
  onProgress?: (progress: ProgressSnapshot) => void
): Promise<DecodeBatchResult> {
  const successes: DecodeSuccess[] = [];
  const failures: DecodeFailure[] = [];
  const allOutputFiles: DecodedOutputFile[] = [];
  const total = entries.length;

  const emitProgress = (snapshot: Partial<ProgressSnapshot>) => {
    const completed = snapshot.completed ?? 0;
    const percent = total > 0 ? Math.round((completed / total) * 100) : 0;
    onProgress?.({
      ...EMPTY_PROGRESS,
      total,
      percent,
      ...snapshot,
      completed,
    });
  };

  emitProgress({ phase: "reading", completed: 0, currentLabel: total > 0 ? entries[0].displayName : "" });

  for (let index = 0; index < entries.length; index += 1) {
    const entry = entries[index];

    try {
      emitProgress({
        phase: "reading",
        completed: index,
        currentLabel: entry.displayName,
      });

      const inputBytes = await entry.loadBytes();

      emitProgress({
        phase: "decoding",
        completed: index,
        currentLabel: entry.displayName,
      });

      const normalizedName = entry.entryName.replace(/\.gzip$/i, ".gz");
      const binaryBytes = normalizedName.endsWith(".gz")
        ? await gunzipArrayBuffer(inputBytes)
        : inputBytes;

      const baseName = normalizeRawOutputBaseName(normalizedName);
      const outputFiles = await decodeThresholdEncodedBinaryToSensorCsvFiles(binaryBytes, baseName);

      if (outputFiles.length === 0) {
        failures.push({
          entryName: entry.entryName,
          reason: "Decoder returned no CSV files.",
        });
      } else {
        successes.push({ entryName: entry.entryName, outputFiles });
        allOutputFiles.push(...outputFiles);
      }
    } catch (error) {
      failures.push({
        entryName: entry.entryName,
        reason: error instanceof Error ? error.message : "Unknown decode error.",
      });
    }
  }

  let downloadedFileName: string | undefined;

  if (allOutputFiles.length > 0) {
    emitProgress({
      phase: "packaging",
      completed: total,
      currentLabel: "Preparing CSV download",
    });

    const artifact = await buildDownloadArtifact(allOutputFiles);
    triggerBlobDownload(artifact.blob, artifact.fileName);
    downloadedFileName = artifact.fileName;
  }

  emitProgress({
    phase: "done",
    completed: total,
    currentLabel: downloadedFileName ? `Downloaded ${downloadedFileName}` : "No decoded files were generated",
  });

  return {
    successes,
    failures,
    downloadedFileName,
  };
}

export async function readMetadataPreviewFromZip(file: File) {
  const archive = await JSZip.loadAsync(file);
  const metadataEntry = archive.file(/export_metadata\.txt$/i)[0];
  if (!metadataEntry) return "";
  return await readTextFromBlob(await metadataEntry.async("blob"));
}
