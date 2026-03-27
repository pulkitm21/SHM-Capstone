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

const TEMP_SCALE = 100;
const ACCEL_SCALE = 10000;
const INCLIN_SCALE = 10000;
const TS_SCALE = 1_000_000;

const FLAG_ACCEL = 0x01;
const FLAG_INCLIN = 0x02;
const FLAG_TEMP = 0x04;
const SENTINEL = 0xff;

const FORMAT_V1 = 1;
const FORMAT_V2 = 2;
const FORMAT_V3 = 3;

const INT32_NAN_SENTINEL = -2147483648;
const CHANGED_NAN_X = 0x10;
const CHANGED_NAN_Y = 0x20;
const CHANGED_NAN_Z = 0x40;
const CHANGED_NAN_TEMP = 0x10;

type SensorState = {
  tsUs: number;
  tsDeltaPrev: number;
  xyzPrev: [number, number, number];
  valPrev: number;
};

type DecodeState = {
  accel: SensorState;
  inclin: SensorState;
  temp: SensorState;
};

type AccelSample = {
  ts: number;
  x: number | null;
  y: number | null;
  z: number | null;
};

type InclinSample = {
  ts: number;
  roll: number | null;
  pitch: number | null;
  yaw: number | null;
};

type TempSample = {
  ts: number;
  tempC: number | null;
};

type RecordEntry = {
  recordIndex: number;
  recordType: "ABSOLUTE" | "DELTA";
  accelSamples: AccelSample[];
  inclin: InclinSample[];
  temp: TempSample | null;
};

type SensorCollections = {
  accel: AccelSample[];
  inclin: InclinSample[];
  temp: TempSample[];
};

class BinaryReader {
  private view: DataView;
  private offset = 0;

  constructor(buffer: ArrayBuffer) {
    this.view = new DataView(buffer);
  }

  get position() {
    return this.offset;
  }

  get length() {
    return this.view.byteLength;
  }

  hasRemaining() {
    return this.offset < this.view.byteLength;
  }

  seekRelative(delta: number) {
    const next = this.offset + delta;
    if (next < 0 || next > this.view.byteLength) {
      throw new Error(`Seek out of range at byte ${this.offset}.`);
    }
    this.offset = next;
  }

  private ensure(bytes: number, label: string) {
    if (this.offset + bytes > this.view.byteLength) {
      throw new Error(`EOF reading ${label} at byte ${this.offset}.`);
    }
  }

  readUint8(label: string) {
    this.ensure(1, label);
    const value = this.view.getUint8(this.offset);
    this.offset += 1;
    return value;
  }

  readInt16(label: string) {
    this.ensure(2, label);
    const value = this.view.getInt16(this.offset, true);
    this.offset += 2;
    return value;
  }

  readInt32(label: string) {
    this.ensure(4, label);
    const value = this.view.getInt32(this.offset, true);
    this.offset += 4;
    return value;
  }

  readInt64(label: string) {
    this.ensure(8, label);
    const value = Number(this.view.getBigInt64(this.offset, true));
    this.offset += 8;
    return value;
  }
}

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

function freshSensorState(): SensorState {
  return {
    tsUs: 0,
    tsDeltaPrev: 0,
    xyzPrev: [0, 0, 0],
    valPrev: 0,
  };
}

function freshDecodeState(): DecodeState {
  return {
    accel: freshSensorState(),
    inclin: freshSensorState(),
    temp: freshSensorState(),
  };
}

function formatTimestampSeconds(tsSeconds: number) {
  return tsSeconds.toFixed(6);
}

function formatOptionalValue(value: number | null, decimals: number) {
  return value == null ? "" : value.toFixed(decimals);
}

function reconstructAbsTs(reader: BinaryReader, sensorState: SensorState) {
  const tsUs = reader.readInt64("absolute timestamp");
  sensorState.tsUs = tsUs;
  return tsUs;
}

function reconstructV1Ts(reader: BinaryReader, sensorState: SensorState) {
  const dod = reader.readInt32("v1 dod_ts");
  const deltaUs = sensorState.tsDeltaPrev + dod;
  const tsUs = sensorState.tsUs + deltaUs;
  sensorState.tsUs = tsUs;
  sensorState.tsDeltaPrev = deltaUs;
  return tsUs;
}

// Decode absolute accelerometer bursts and preserve previous non-NaN axis values.
function decodeAccelAbsolute(reader: BinaryReader, state: DecodeState) {
  const count = reader.readUint8("accel count");
  const samples: AccelSample[] = [];
  const prev = [...state.accel.xyzPrev] as [number, number, number];

  for (let index = 0; index < count; index += 1) {
    const tsUs = reconstructAbsTs(reader, state.accel);
    const rawValues: [number, number, number] = [
      reader.readInt32("accel x"),
      reader.readInt32("accel y"),
      reader.readInt32("accel z"),
    ];

    const values: [number | null, number | null, number | null] = [null, null, null];

    for (let axis = 0; axis < 3; axis += 1) {
      const raw = rawValues[axis];
      if (raw !== INT32_NAN_SENTINEL) {
        prev[axis] = raw;
        values[axis] = raw / ACCEL_SCALE;
      }
    }

    samples.push({
      ts: tsUs / TS_SCALE,
      x: values[0],
      y: values[1],
      z: values[2],
    });
  }

  state.accel.xyzPrev = prev;
  return samples;
}

// Decode legacy v1 accelerometer delta samples.
function decodeAccelDeltaV1(reader: BinaryReader, state: DecodeState) {
  const count = reader.readUint8("accel count");
  const samples: AccelSample[] = [];
  let [prevX, prevY, prevZ] = state.accel.xyzPrev;

  for (let index = 0; index < count; index += 1) {
    const tsUs = reconstructV1Ts(reader, state.accel);
    const dx = reader.readInt16("accel dx");
    const dy = reader.readInt16("accel dy");
    const dz = reader.readInt16("accel dz");

    prevX += dx;
    prevY += dy;
    prevZ += dz;

    samples.push({
      ts: tsUs / TS_SCALE,
      x: prevX / ACCEL_SCALE,
      y: prevY / ACCEL_SCALE,
      z: prevZ / ACCEL_SCALE,
    });
  }

  state.accel.xyzPrev = [prevX, prevY, prevZ];
  return samples;
}

// Decode v2 and v3 accelerometer deltas including optional NaN flags.
function decodeAccelDelta(reader: BinaryReader, state: DecodeState, formatVersion: number) {
  const count = reader.readUint8("accel count");
  const samples: AccelSample[] = [];
  const prev = [...state.accel.xyzPrev] as [number, number, number];

  for (let index = 0; index < count; index += 1) {
    const changed = reader.readUint8("accel changed");
    const lowMask = changed & 0x0f;
    const nanMask = formatVersion >= FORMAT_V3 ? changed & 0x70 : 0;

    if (lowMask & 0x01) {
      state.accel.tsUs += reader.readInt32("accel delta_ts");
    }

    const values: [number | null, number | null, number | null] = [null, null, null];
    const bits = [0x02, 0x04, 0x08] as const;
    const nanBits = [CHANGED_NAN_X, CHANGED_NAN_Y, CHANGED_NAN_Z] as const;

    for (let axis = 0; axis < 3; axis += 1) {
      if (nanMask & nanBits[axis]) {
        values[axis] = null;
        continue;
      }

      if (lowMask & bits[axis]) {
        prev[axis] += reader.readInt16("accel delta");
      }

      values[axis] = prev[axis] / ACCEL_SCALE;
    }

    samples.push({
      ts: state.accel.tsUs / TS_SCALE,
      x: values[0],
      y: values[1],
      z: values[2],
    });
  }

  state.accel.xyzPrev = prev;
  return samples;
}

// Decode absolute inclination records for v1/v2 single samples and v3 bursts.
function decodeInclinAbsolute(reader: BinaryReader, state: DecodeState, formatVersion: number) {
  const samples: InclinSample[] = [];
  const prev = [...state.inclin.xyzPrev] as [number, number, number];

  const count = formatVersion >= FORMAT_V3 ? reader.readUint8("inclin count") : 1;

  for (let index = 0; index < count; index += 1) {
    const tsUs = reconstructAbsTs(reader, state.inclin);
    const rawValues: [number, number, number] = [
      reader.readInt32("inclin roll"),
      reader.readInt32("inclin pitch"),
      reader.readInt32("inclin yaw"),
    ];

    const values: [number | null, number | null, number | null] = [null, null, null];

    for (let axis = 0; axis < 3; axis += 1) {
      const raw = rawValues[axis];
      if (formatVersion >= FORMAT_V3 && raw === INT32_NAN_SENTINEL) {
        values[axis] = null;
        continue;
      }
      prev[axis] = raw;
      values[axis] = raw / INCLIN_SCALE;
    }

    samples.push({
      ts: tsUs / TS_SCALE,
      roll: values[0],
      pitch: values[1],
      yaw: values[2],
    });
  }

  state.inclin.xyzPrev = prev;
  return samples;
}

// Decode legacy v1 inclination delta samples.
function decodeInclinDeltaV1(reader: BinaryReader, state: DecodeState): InclinSample[] {
  const tsUs = reconstructV1Ts(reader, state.inclin);
  let [prevRoll, prevPitch, prevYaw] = state.inclin.xyzPrev;

  prevRoll += reader.readInt16("inclin dr");
  prevPitch += reader.readInt16("inclin dp");
  prevYaw += reader.readInt16("inclin dyaw");

  state.inclin.xyzPrev = [prevRoll, prevPitch, prevYaw];

  return [{
    ts: tsUs / TS_SCALE,
    roll: prevRoll / INCLIN_SCALE,
    pitch: prevPitch / INCLIN_SCALE,
    yaw: prevYaw / INCLIN_SCALE,
  }];
}

// Decode v2 and v3 inclination deltas including v3 bursts and NaN flags.
function decodeInclinDelta(reader: BinaryReader, state: DecodeState, formatVersion: number): InclinSample[] {
  const samples: InclinSample[] = [];
  const prev = [...state.inclin.xyzPrev] as [number, number, number];
  const count = formatVersion >= FORMAT_V3 ? reader.readUint8("inclin count") : 1;

  for (let index = 0; index < count; index += 1) {
    const changed = reader.readUint8("inclin changed");
    const lowMask = changed & 0x0f;
    const nanMask = formatVersion >= FORMAT_V3 ? changed & 0x70 : 0;

    if (lowMask & 0x01) {
      state.inclin.tsUs += reader.readInt32("inclin delta_ts");
    }

    const values: [number | null, number | null, number | null] = [null, null, null];
    const bits = [0x02, 0x04, 0x08] as const;
    const nanBits = [CHANGED_NAN_X, CHANGED_NAN_Y, CHANGED_NAN_Z] as const;

    for (let axis = 0; axis < 3; axis += 1) {
      if (nanMask & nanBits[axis]) {
        values[axis] = null;
        continue;
      }

      if (lowMask & bits[axis]) {
        prev[axis] += reader.readInt16("inclin delta");
      }

      values[axis] = prev[axis] / INCLIN_SCALE;
    }

    samples.push({
      ts: state.inclin.tsUs / TS_SCALE,
      roll: values[0],
      pitch: values[1],
      yaw: values[2],
    });
  }

  state.inclin.xyzPrev = prev;
  return samples;
}

// Decode absolute temperature samples and preserve previous non-NaN values.
function decodeTempAbsolute(reader: BinaryReader, state: DecodeState): TempSample {
  const tsUs = reconstructAbsTs(reader, state.temp);
  const tempRaw = reader.readInt32("temp value");

  if (tempRaw === INT32_NAN_SENTINEL) {
    return {
      ts: tsUs / TS_SCALE,
      tempC: null,
    };
  }

  state.temp.valPrev = tempRaw;
  return {
    ts: tsUs / TS_SCALE,
    tempC: tempRaw / TEMP_SCALE,
  };
}

// Decode legacy v1 temperature delta samples.
function decodeTempDeltaV1(reader: BinaryReader, state: DecodeState): TempSample {
  const tsUs = reconstructV1Ts(reader, state.temp);
  state.temp.valPrev += reader.readInt16("temp delta");

  return {
    ts: tsUs / TS_SCALE,
    tempC: state.temp.valPrev / TEMP_SCALE,
  };
}

// Decode v2 and v3 temperature deltas including explicit NaN flags in v3.
function decodeTempDelta(reader: BinaryReader, state: DecodeState, formatVersion: number): TempSample {
  const changed = reader.readUint8("temp changed");
  const lowMask = changed & 0x0f;

  if (lowMask & 0x01) {
    state.temp.tsUs += reader.readInt32("temp delta_ts");
  }

  const isNan = formatVersion >= FORMAT_V3 && Boolean(changed & CHANGED_NAN_TEMP);
  if (isNan) {
    return {
      ts: state.temp.tsUs / TS_SCALE,
      tempC: null,
    };
  }

  if (lowMask & 0x02) {
    state.temp.valPrev += reader.readInt16("temp delta");
  }

  return {
    ts: state.temp.tsUs / TS_SCALE,
    tempC: state.temp.valPrev / TEMP_SCALE,
  };
}

function isTruncatedTailError(error: unknown) {
  return error instanceof Error && error.message.startsWith("EOF reading ");
}

// Decode one binary file into the same record model as the Python decoder.
function decodeBinaryRecords(bytes: ArrayBuffer) {
  const reader = new BinaryReader(bytes);
  const state = freshDecodeState();
  const records: RecordEntry[] = [];

  if (!reader.hasRemaining()) {
    return records;
  }

  let formatVersion: number;
  try {
    formatVersion = reader.readUint8("file format version");
  } catch (error) {
    if (isTruncatedTailError(error)) {
      return records;
    }
    throw error;
  }

  if (formatVersion !== FORMAT_V1 && formatVersion !== FORMAT_V2 && formatVersion !== FORMAT_V3) {
    reader.seekRelative(-1);
    formatVersion = FORMAT_V1;
  }

  let recordIndex = 0;

  while (reader.hasRemaining()) {
    try {
      const headerOrSentinel = reader.readUint8("record header");
      const isAbsolute = headerOrSentinel === SENTINEL;
      const header = isAbsolute ? reader.readUint8("absolute header") : headerOrSentinel;

      const record: RecordEntry = {
        recordIndex,
        recordType: isAbsolute ? "ABSOLUTE" : "DELTA",
        accelSamples: [],
        inclin: [],
        temp: null,
      };

      if (isAbsolute) {
        if (header & FLAG_ACCEL) record.accelSamples = decodeAccelAbsolute(reader, state);
        if (header & FLAG_INCLIN) record.inclin = decodeInclinAbsolute(reader, state, formatVersion);
        if (header & FLAG_TEMP) record.temp = decodeTempAbsolute(reader, state);
      } else if (formatVersion === FORMAT_V1) {
        if (header & FLAG_ACCEL) record.accelSamples = decodeAccelDeltaV1(reader, state);
        if (header & FLAG_INCLIN) record.inclin = decodeInclinDeltaV1(reader, state);
        if (header & FLAG_TEMP) record.temp = decodeTempDeltaV1(reader, state);
      } else {
        if (header & FLAG_ACCEL) record.accelSamples = decodeAccelDelta(reader, state, formatVersion);
        if (header & FLAG_INCLIN) record.inclin = decodeInclinDelta(reader, state, formatVersion);
        if (header & FLAG_TEMP) record.temp = decodeTempDelta(reader, state, formatVersion);
      }

      records.push(record);
      recordIndex += 1;
    } catch (error) {
      if (isTruncatedTailError(error)) {
        const message = error instanceof Error ? error.message : String(error);
        console.warn(`Decoder stopped at truncated record #${recordIndex}: ${message}`);
        break;
      }
      throw error;
    }
  }

  return records;
}

// Flatten decoded records into per-sensor collections for CSV export.
function collectSensorSamples(records: RecordEntry[]): SensorCollections {
  const sensors: SensorCollections = {
    accel: [],
    inclin: [],
    temp: [],
  };

  for (const record of records) {
    if (record.accelSamples.length > 0) {
      sensors.accel.push(...record.accelSamples);
    }

    if (record.inclin.length > 0) {
      sensors.inclin.push(...record.inclin);
    }

    if (record.temp) {
      sensors.temp.push(record.temp);
    }
  }

  return sensors;
}

// Build accelerometer CSV output with blank cells for NaN values.
function buildAccelCsv(samples: AccelSample[]) {
  const lines = ["timestamp_s,x_g,y_g,z_g"];
  for (const sample of samples) {
    lines.push(
      [
        formatTimestampSeconds(sample.ts),
        formatOptionalValue(sample.x, 4),
        formatOptionalValue(sample.y, 4),
        formatOptionalValue(sample.z, 4),
      ].join(",")
    );
  }
  return `${lines.join("\n")}\n`;
}

// Build inclinometer CSV output with blank cells for NaN values.
function buildInclinCsv(samples: InclinSample[]) {
  const lines = ["timestamp_s,roll_deg,pitch_deg,yaw_deg"];
  for (const sample of samples) {
    lines.push(
      [
        formatTimestampSeconds(sample.ts),
        formatOptionalValue(sample.roll, 4),
        formatOptionalValue(sample.pitch, 4),
        formatOptionalValue(sample.yaw, 4),
      ].join(",")
    );
  }
  return `${lines.join("\n")}\n`;
}

// Build temperature CSV output with blank cells for NaN values.
function buildTempCsv(samples: TempSample[]) {
  const lines = ["timestamp_s,temp_c"];
  for (const sample of samples) {
    lines.push([formatTimestampSeconds(sample.ts), formatOptionalValue(sample.tempC, 2)].join(","));
  }
  return `${lines.join("\n")}\n`;
}

// Decode raw binary bytes into per-sensor CSV files.
async function decodeThresholdEncodedBinaryToSensorCsvFiles(
  bytes: ArrayBuffer,
  baseName: string
): Promise<DecodedOutputFile[]> {
  const records = decodeBinaryRecords(bytes);
  const sensors = collectSensorSamples(records);
  const outputFiles: DecodedOutputFile[] = [];

  if (sensors.accel.length > 0) {
    outputFiles.push({
      fileName: `${baseName}_accelerometer.csv`,
      content: buildAccelCsv(sensors.accel),
    });
  }

  if (sensors.inclin.length > 0) {
    outputFiles.push({
      fileName: `${baseName}_inclinometer.csv`,
      content: buildInclinCsv(sensors.inclin),
    });
  }

  if (sensors.temp.length > 0) {
    outputFiles.push({
      fileName: `${baseName}_temperature.csv`,
      content: buildTempCsv(sensors.temp),
    });
  }

  return outputFiles;
}

// Collect standalone raw files and raw files nested inside ZIP archives.
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

// Decode all queued files and trigger a CSV or ZIP download in the browser.
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
      const binaryBytes = normalizedName.endsWith(".gz") ? await gunzipArrayBuffer(inputBytes) : inputBytes;
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

// Read optional export metadata from a selected ZIP file for preview in the UI.
export async function readMetadataPreviewFromZip(file: File) {
  const archive = await JSZip.loadAsync(file);
  const metadataEntry = archive.file(/export_metadata\.txt$/i)[0];
  if (!metadataEntry) return "";
  return await readTextFromBlob(await metadataEntry.async("blob"));
}
