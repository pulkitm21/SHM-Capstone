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

export type DecodeOutputMode = "sensor" | "node";

export const EMPTY_PROGRESS: ProgressSnapshot = {
  phase: "idle",
  completed: 0,
  total: 0,
  percent: 0,
  currentLabel: "",
};

// Accept processed storage files and raw capture files.
const SUPPORTED_FILE_PATTERN = /\.(?:bin(?:\.gz)?|rawbin)$/i;

const TEMP_SCALE = 100;
const ACCEL_SCALE = 10000;
const INCLIN_SCALE = 10000;
const TS_SCALE = 1_000_000;

const FLAG_ACCEL = 0x01;
const FLAG_INCLIN = 0x02;
const FLAG_TEMP = 0x04;
const SENTINEL = 0xff;

const FORMAT_V3 = 3;

const INT32_NAN_SENTINEL = -2147483648;
const CHANGED_NAN_X = 0x10;
const CHANGED_NAN_Y = 0x20;
const CHANGED_NAN_Z = 0x40;
const CHANGED_NAN_TEMP = 0x10;

type SensorState = {
  tsUs: number;
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
  tsUs: number;
  x: number | null;
  y: number | null;
  z: number | null;
};

type InclinSample = {
  ts: number;
  tsUs: number;
  roll: number | null;
  pitch: number | null;
  yaw: number | null;
};

type TempSample = {
  ts: number;
  tsUs: number;
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

type NodeCsvRow = {
  tsUs: number;
  sensorType: "accelerometer" | "inclinometer" | "temperature";
  x: number | null;
  y: number | null;
  z: number | null;
  roll: number | null;
  pitch: number | null;
  yaw: number | null;
  tempC: number | null;
};

class BinaryReader {
  private view: DataView;
  private offset = 0;

  constructor(buffer: ArrayBuffer) {
    this.view = new DataView(buffer);
  }

  hasRemaining() {
    return this.offset < this.view.byteLength;
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

  readUint32(label: string) {
    this.ensure(4, label);
    const value = this.view.getUint32(this.offset, true);
    this.offset += 4;
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

  readUint64(label: string) {
    this.ensure(8, label);
    const value = Number(this.view.getBigUint64(this.offset, true));
    this.offset += 8;
    return value;
  }

  readBytes(length: number, label: string) {
    this.ensure(length, label);
    const start = this.offset;
    const end = this.offset + length;
    this.offset = end;
    return new Uint8Array(this.view.buffer.slice(start, end));
  }
}

function isSupportedDecoderFileName(name: string) {
  return SUPPORTED_FILE_PATTERN.test(name);
}

// Infer the decoder path from the uploaded file extension.
function inferDecoderInputKind(name: string): "processed" | "raw" {
  return name.toLowerCase().endsWith(".rawbin") ? "raw" : "processed";
}

// Strip only the known decoder file suffixes from output names.
function normalizeRawOutputBaseName(name: string) {
  return name
    .replace(/\.bin\.gz$/i, "")
    .replace(/\.rawbin$/i, "")
    .replace(/\.bin$/i, "");
}

function arrayBufferToBlob(
  buffer: ArrayBuffer,
  type = "application/octet-stream"
) {
  return new Blob([buffer], { type });
}

// Decompress .bin.gz files in the browser.
async function gunzipArrayBuffer(input: ArrayBuffer): Promise<ArrayBuffer> {
  if (typeof DecompressionStream === "undefined") {
    throw new Error(
      "This browser does not support gzip decompression in the decoder page."
    );
  }

  const sourceBlob = arrayBufferToBlob(input);
  const decompressedStream = sourceBlob
    .stream()
    .pipeThrough(new DecompressionStream("gzip"));
  const response = new Response(decompressedStream);
  return await response.arrayBuffer();
}

// Trigger the browser download for the generated artifact.
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

// Return a single CSV or a ZIP depending on how many files were generated.
async function buildDownloadArtifact(
  outputFiles: DecodedOutputFile[],
  outputMode: DecodeOutputMode
) {
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
    fileName: `decoded_${outputMode}_csv_${new Date()
      .toISOString()
      .slice(0, 10)}.zip`,
    blob,
  };
}

function freshSensorState(): SensorState {
  return {
    tsUs: 0,
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

function formatTimestampIso(tsUs: number) {
  return new Date(tsUs / 1000).toISOString();
}

function formatOptionalValue(value: number | null, decimals: number) {
  return value == null ? "" : value.toFixed(decimals);
}


function isFiniteNumber(value: unknown): value is number {
  return typeof value === "number" && Number.isFinite(value);
}

function normalizeNumericValue(value: unknown): number | null {
  if (value == null) {
    return null;
  }

  if (typeof value === "number") {
    return Number.isFinite(value) ? value : null;
  }

  if (typeof value === "string") {
    const trimmed = value.trim();
    if (!trimmed) {
      return null;
    }

    const parsed = Number(trimmed);
    return Number.isFinite(parsed) ? parsed : null;
  }

  return null;
}

function isTimestampLike(value: unknown) {
  if (typeof value === "string") {
    const trimmed = value.trim();
    if (!trimmed) {
      return false;
    }

    if (!Number.isNaN(Date.parse(trimmed))) {
      return true;
    }

    return Number.isFinite(Number(trimmed));
  }

  return isFiniteNumber(value);
}

function parseTimestampValueToUs(value: unknown, fallbackUs: number) {
  if (typeof value === "string") {
    const trimmed = value.trim();
    if (!trimmed) {
      return fallbackUs;
    }

    const parsedDate = Date.parse(trimmed);
    if (!Number.isNaN(parsedDate)) {
      return Math.trunc(parsedDate * 1000);
    }

    const numeric = Number(trimmed);
    if (!Number.isFinite(numeric)) {
      return fallbackUs;
    }

    return parseTimestampValueToUs(numeric, fallbackUs);
  }

  if (!isFiniteNumber(value)) {
    return fallbackUs;
  }

  if (value >= 1e14) {
    return Math.trunc(value);
  }

  if (value >= 1e11) {
    return Math.trunc(value * 1000);
  }

  if (value >= 1e9) {
    return Math.trunc(value * TS_SCALE);
  }

  return fallbackUs;
}

function parseRawPacketTimestampUs(packet: Record<string, unknown>, fallbackUs: number) {
  const packetTs = packet.t ?? packet.ts ?? packet.timestamp;
  return parseTimestampValueToUs(packetTs, fallbackUs);
}

function buildRawAccelSample(sample: unknown[], fallbackUs: number): AccelSample | null {
  if (sample.length >= 4 && isTimestampLike(sample[0])) {
    return {
      ts: parseTimestampValueToUs(sample[0], fallbackUs) / TS_SCALE,
      tsUs: parseTimestampValueToUs(sample[0], fallbackUs),
      x: normalizeNumericValue(sample[1]),
      y: normalizeNumericValue(sample[2]),
      z: normalizeNumericValue(sample[3]),
    };
  }

  if (sample.length >= 3) {
    return {
      ts: fallbackUs / TS_SCALE,
      tsUs: fallbackUs,
      x: normalizeNumericValue(sample[0]),
      y: normalizeNumericValue(sample[1]),
      z: normalizeNumericValue(sample[2]),
    };
  }

  return null;
}

function buildRawInclinSample(sample: unknown[], fallbackUs: number): InclinSample | null {
  if (sample.length >= 4 && isTimestampLike(sample[0])) {
    return {
      ts: parseTimestampValueToUs(sample[0], fallbackUs) / TS_SCALE,
      tsUs: parseTimestampValueToUs(sample[0], fallbackUs),
      roll: normalizeNumericValue(sample[1]),
      pitch: normalizeNumericValue(sample[2]),
      yaw: normalizeNumericValue(sample[3]),
    };
  }

  if (sample.length >= 3) {
    return {
      ts: fallbackUs / TS_SCALE,
      tsUs: fallbackUs,
      roll: normalizeNumericValue(sample[0]),
      pitch: normalizeNumericValue(sample[1]),
      yaw: normalizeNumericValue(sample[2]),
    };
  }

  return null;
}

function buildRawTempSample(value: unknown, fallbackUs: number): TempSample | null {
  if (value == null) {
    return null;
  }

  if (Array.isArray(value)) {
    if (value.length >= 2 && isTimestampLike(value[0])) {
      const tsUs = parseTimestampValueToUs(value[0], fallbackUs);
      return {
        ts: tsUs / TS_SCALE,
        tsUs,
        tempC: normalizeNumericValue(value[1]),
      };
    }

    if (value.length >= 1) {
      return {
        ts: fallbackUs / TS_SCALE,
        tsUs: fallbackUs,
        tempC: normalizeNumericValue(value[value.length - 1]),
      };
    }

    return null;
  }

  return {
    ts: fallbackUs / TS_SCALE,
    tsUs: fallbackUs,
    tempC: normalizeNumericValue(value),
  };
}

function normalizeRawInclinEntries(value: unknown): unknown[][] {
  if (!Array.isArray(value) || value.length === 0) {
    return [];
  }

  if (Array.isArray(value[0])) {
    return value.filter((entry): entry is unknown[] => Array.isArray(entry));
  }

  return [value];
}

function normalizeRawAccelEntries(value: unknown): unknown[][] {
  if (!Array.isArray(value) || value.length === 0) {
    return [];
  }

  if (Array.isArray(value[0])) {
    return value.filter((entry): entry is unknown[] => Array.isArray(entry));
  }

  return [value];
}

function decodeRawArchiveRecords(bytes: ArrayBuffer) {
  const reader = new BinaryReader(bytes);
  const textDecoder = new TextDecoder();
  const records: RecordEntry[] = [];
  let recordIndex = 0;

  while (reader.hasRemaining()) {
    try {
      const recvNs = reader.readUint64("raw receive timestamp");
      const payloadLength = reader.readUint32("raw payload length");
      const payloadBytes = reader.readBytes(payloadLength, "raw payload");
      const fallbackUs = Math.trunc(recvNs / 1000);
      const payloadText = textDecoder.decode(payloadBytes);
      const packet = JSON.parse(payloadText) as Record<string, unknown>;
      const packetTsUs = parseRawPacketTimestampUs(packet, fallbackUs);

      const record: RecordEntry = {
        recordIndex,
        recordType: "ABSOLUTE",
        accelSamples: [],
        inclin: [],
        temp: null,
      };

      for (const sample of normalizeRawAccelEntries(packet.a)) {
        const accelSample = buildRawAccelSample(sample, packetTsUs);
        if (accelSample) {
          record.accelSamples.push(accelSample);
        }
      }

      for (const sample of normalizeRawInclinEntries(packet.i)) {
        const inclinSample = buildRawInclinSample(sample, packetTsUs);
        if (inclinSample) {
          record.inclin.push(inclinSample);
        }
      }

      record.temp = buildRawTempSample(packet.T, packetTsUs);

      if (
        record.accelSamples.length > 0 ||
        record.inclin.length > 0 ||
        record.temp
      ) {
        records.push(record);
        recordIndex += 1;
      }
    } catch (error) {
      if (isTruncatedTailError(error)) {
        const message = error instanceof Error ? error.message : String(error);
        console.warn(`Raw decoder stopped at truncated record #${recordIndex}: ${message}`);
        break;
      }

      console.warn(
        `Raw decoder skipped record #${recordIndex}: ${
          error instanceof Error ? error.message : String(error)
        }`
      );
      recordIndex += 1;
    }
  }

  return records;
}

function reconstructAbsTs(reader: BinaryReader, sensorState: SensorState) {
  const tsUs = reader.readInt64("absolute timestamp");
  sensorState.tsUs = tsUs;
  return tsUs;
}

// Decode absolute accelerometer bursts.
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

    const values: [number | null, number | null, number | null] = [
      null,
      null,
      null,
    ];

    for (let axis = 0; axis < 3; axis += 1) {
      const raw = rawValues[axis];
      if (raw !== INT32_NAN_SENTINEL) {
        prev[axis] = raw;
        values[axis] = raw / ACCEL_SCALE;
      }
    }

    samples.push({
      ts: tsUs / TS_SCALE,
      tsUs,
      x: values[0],
      y: values[1],
      z: values[2],
    });
  }

  state.accel.xyzPrev = prev;
  return samples;
}

// Decode V3 accelerometer deltas with NaN flags.
function decodeAccelDelta(reader: BinaryReader, state: DecodeState) {
  const count = reader.readUint8("accel count");
  const samples: AccelSample[] = [];
  const prev = [...state.accel.xyzPrev] as [number, number, number];

  for (let index = 0; index < count; index += 1) {
    const changed = reader.readUint8("accel changed");
    const lowMask = changed & 0x0f;
    const nanMask = changed & 0x70;

    if (lowMask & 0x01) {
      state.accel.tsUs += reader.readInt32("accel delta_ts");
    }

    const values: [number | null, number | null, number | null] = [
      null,
      null,
      null,
    ];
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
      tsUs: state.accel.tsUs,
      x: values[0],
      y: values[1],
      z: values[2],
    });
  }

  state.accel.xyzPrev = prev;
  return samples;
}

// Decode absolute inclinometer bursts.
function decodeInclinAbsolute(reader: BinaryReader, state: DecodeState) {
  const samples: InclinSample[] = [];
  const prev = [...state.inclin.xyzPrev] as [number, number, number];
  const count = reader.readUint8("inclin count");

  for (let index = 0; index < count; index += 1) {
    const tsUs = reconstructAbsTs(reader, state.inclin);
    const rawValues: [number, number, number] = [
      reader.readInt32("inclin roll"),
      reader.readInt32("inclin pitch"),
      reader.readInt32("inclin yaw"),
    ];

    const values: [number | null, number | null, number | null] = [
      null,
      null,
      null,
    ];

    for (let axis = 0; axis < 3; axis += 1) {
      const raw = rawValues[axis];
      if (raw === INT32_NAN_SENTINEL) {
        values[axis] = null;
        continue;
      }

      prev[axis] = raw;
      values[axis] = raw / INCLIN_SCALE;
    }

    samples.push({
      ts: tsUs / TS_SCALE,
      tsUs,
      roll: values[0],
      pitch: values[1],
      yaw: values[2],
    });
  }

  state.inclin.xyzPrev = prev;
  return samples;
}

// Decode V3 inclinometer deltas with NaN flags.
function decodeInclinDelta(reader: BinaryReader, state: DecodeState): InclinSample[] {
  const samples: InclinSample[] = [];
  const prev = [...state.inclin.xyzPrev] as [number, number, number];
  const count = reader.readUint8("inclin count");

  for (let index = 0; index < count; index += 1) {
    const changed = reader.readUint8("inclin changed");
    const lowMask = changed & 0x0f;
    const nanMask = changed & 0x70;

    if (lowMask & 0x01) {
      state.inclin.tsUs += reader.readInt32("inclin delta_ts");
    }

    const values: [number | null, number | null, number | null] = [
      null,
      null,
      null,
    ];
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
      tsUs: state.inclin.tsUs,
      roll: values[0],
      pitch: values[1],
      yaw: values[2],
    });
  }

  state.inclin.xyzPrev = prev;
  return samples;
}

// Decode absolute temperature samples.
function decodeTempAbsolute(reader: BinaryReader, state: DecodeState): TempSample {
  const tsUs = reconstructAbsTs(reader, state.temp);
  const tempRaw = reader.readInt32("temp value");

  if (tempRaw === INT32_NAN_SENTINEL) {
    return {
      ts: tsUs / TS_SCALE,
      tsUs,
      tempC: null,
    };
  }

  state.temp.valPrev = tempRaw;
  return {
    ts: tsUs / TS_SCALE,
    tsUs,
    tempC: tempRaw / TEMP_SCALE,
  };
}

// Decode V3 temperature deltas with NaN flags.
function decodeTempDelta(reader: BinaryReader, state: DecodeState): TempSample {
  const changed = reader.readUint8("temp changed");
  const lowMask = changed & 0x0f;

  if (lowMask & 0x01) {
    state.temp.tsUs += reader.readInt32("temp delta_ts");
  }

  const isNan = Boolean(changed & CHANGED_NAN_TEMP);
  if (isNan) {
    return {
      ts: state.temp.tsUs / TS_SCALE,
      tsUs: state.temp.tsUs,
      tempC: null,
    };
  }

  if (lowMask & 0x02) {
    state.temp.valPrev += reader.readInt16("temp delta");
  }

  return {
    ts: state.temp.tsUs / TS_SCALE,
    tsUs: state.temp.tsUs,
    tempC: state.temp.valPrev / TEMP_SCALE,
  };
}

function isTruncatedTailError(error: unknown) {
  return error instanceof Error && error.message.startsWith("EOF reading ");
}

// Decode one V3 binary file into record entries.
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

  if (formatVersion !== FORMAT_V3) {
    throw new Error(
      `Unsupported decoder format version ${formatVersion}. Expected V3.`
    );
  }

  let recordIndex = 0;

  while (reader.hasRemaining()) {
    try {
      const headerOrSentinel = reader.readUint8("record header");
      const isAbsolute = headerOrSentinel === SENTINEL;
      const header = isAbsolute
        ? reader.readUint8("absolute header")
        : headerOrSentinel;

      const record: RecordEntry = {
        recordIndex,
        recordType: isAbsolute ? "ABSOLUTE" : "DELTA",
        accelSamples: [],
        inclin: [],
        temp: null,
      };

      if (isAbsolute) {
        if (header & FLAG_ACCEL) {
          record.accelSamples = decodeAccelAbsolute(reader, state);
        }
        if (header & FLAG_INCLIN) {
          record.inclin = decodeInclinAbsolute(reader, state);
        }
        if (header & FLAG_TEMP) {
          record.temp = decodeTempAbsolute(reader, state);
        }
      } else {
        if (header & FLAG_ACCEL) {
          record.accelSamples = decodeAccelDelta(reader, state);
        }
        if (header & FLAG_INCLIN) {
          record.inclin = decodeInclinDelta(reader, state);
        }
        if (header & FLAG_TEMP) {
          record.temp = decodeTempDelta(reader, state);
        }
      }

      records.push(record);
      recordIndex += 1;
    } catch (error) {
      if (isTruncatedTailError(error)) {
        const message = error instanceof Error ? error.message : String(error);
        console.warn(
          `Decoder stopped at truncated record #${recordIndex}: ${message}`
        );
        break;
      }
      throw error;
    }
  }

  return records;
}

// Flatten decoded records into per-sensor collections.
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

// Build one combined node CSV row list.
function collectNodeCsvRows(records: RecordEntry[]): NodeCsvRow[] {
  const rows: NodeCsvRow[] = [];

  for (const record of records) {
    for (const sample of record.accelSamples) {
      rows.push({
        tsUs: sample.tsUs,
        sensorType: "accelerometer",
        x: sample.x,
        y: sample.y,
        z: sample.z,
        roll: null,
        pitch: null,
        yaw: null,
        tempC: null,
      });
    }

    for (const sample of record.inclin) {
      rows.push({
        tsUs: sample.tsUs,
        sensorType: "inclinometer",
        x: null,
        y: null,
        z: null,
        roll: sample.roll,
        pitch: sample.pitch,
        yaw: sample.yaw,
        tempC: null,
      });
    }

    if (record.temp) {
      rows.push({
        tsUs: record.temp.tsUs,
        sensorType: "temperature",
        x: null,
        y: null,
        z: null,
        roll: null,
        pitch: null,
        yaw: null,
        tempC: record.temp.tempC,
      });
    }
  }

  rows.sort((a, b) => a.tsUs - b.tsUs);
  return rows;
}

// Build accelerometer CSV output.
function buildAccelCsv(samples: AccelSample[]) {
  const lines = ["timestamp_iso,timestamp_us,x_g,y_g,z_g"];
  for (const sample of samples) {
    lines.push(
      [
        formatTimestampIso(sample.tsUs),
        sample.tsUs.toString(),
        formatOptionalValue(sample.x, 4),
        formatOptionalValue(sample.y, 4),
        formatOptionalValue(sample.z, 4),
      ].join(",")
    );
  }
  return `${lines.join("\n")}\n`;
}

// Build inclinometer CSV output.
function buildInclinCsv(samples: InclinSample[]) {
  const lines = ["timestamp_iso,timestamp_us,roll_deg,pitch_deg,yaw_deg"];
  for (const sample of samples) {
    lines.push(
      [
        formatTimestampIso(sample.tsUs),
        sample.tsUs.toString(),
        formatOptionalValue(sample.roll, 4),
        formatOptionalValue(sample.pitch, 4),
        formatOptionalValue(sample.yaw, 4),
      ].join(",")
    );
  }
  return `${lines.join("\n")}\n`;
}

// Build temperature CSV output.
function buildTempCsv(samples: TempSample[]) {
  const lines = ["timestamp_iso,timestamp_us,temp_c"];
  for (const sample of samples) {
    lines.push(
      [
        formatTimestampIso(sample.tsUs),
        sample.tsUs.toString(),
        formatOptionalValue(sample.tempC, 2),
      ].join(",")
    );
  }
  return `${lines.join("\n")}\n`;
}

// Build one combined node CSV.
function buildNodeCsv(records: RecordEntry[]) {
  const rows = collectNodeCsvRows(records);
  const lines = [
    "timestamp_iso,timestamp_us,sensor_type,x_g,y_g,z_g,roll_deg,pitch_deg,yaw_deg,temp_c",
  ];

  for (const row of rows) {
    lines.push(
      [
        formatTimestampIso(row.tsUs),
        row.tsUs.toString(),
        row.sensorType,
        formatOptionalValue(row.x, 4),
        formatOptionalValue(row.y, 4),
        formatOptionalValue(row.z, 4),
        formatOptionalValue(row.roll, 4),
        formatOptionalValue(row.pitch, 4),
        formatOptionalValue(row.yaw, 4),
        formatOptionalValue(row.tempC, 2),
      ].join(",")
    );
  }

  return `${lines.join("\n")}\n`;
}

// Decode raw bytes into either sensor CSVs or one node CSV.
async function decodeThresholdEncodedBinaryToCsvFiles(
  bytes: ArrayBuffer,
  baseName: string,
  outputMode: DecodeOutputMode
): Promise<DecodedOutputFile[]> {
  const records = decodeBinaryRecords(bytes);
  const outputFiles: DecodedOutputFile[] = [];

  if (outputMode === "node") {
    if (records.length === 0) {
      return outputFiles;
    }

    outputFiles.push({
      fileName: `${baseName}.csv`,
      content: buildNodeCsv(records),
    });
    return outputFiles;
  }

  const sensors = collectSensorSamples(records);

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


async function decodeRawArchiveBinaryToCsvFiles(
  bytes: ArrayBuffer,
  baseName: string,
  outputMode: DecodeOutputMode
): Promise<DecodedOutputFile[]> {
  const records = decodeRawArchiveRecords(bytes);
  const outputFiles: DecodedOutputFile[] = [];

  if (outputMode === "node") {
    if (records.length === 0) {
      return outputFiles;
    }

    outputFiles.push({
      fileName: `${baseName}.csv`,
      content: buildNodeCsv(records),
    });
    return outputFiles;
  }

  const sensors = collectSensorSamples(records);

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

// Collect supported decoder input files without changing their contents.
export async function collectDecoderInputEntries(files: File[]) {
  const entries: RawInputEntry[] = [];
  const rejectedNames: string[] = [];

  for (const file of files) {
    if (isSupportedDecoderFileName(file.name)) {
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

// Decode all queued files and trigger the browser download.
export async function decodeEntriesToCsv(
  entries: RawInputEntry[],
  outputMode: DecodeOutputMode,
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

  emitProgress({
    phase: "reading",
    completed: 0,
    currentLabel: total > 0 ? entries[0].displayName : "",
  });

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

      const binaryBytes = entry.entryName.toLowerCase().endsWith(".bin.gz")
        ? await gunzipArrayBuffer(inputBytes)
        : inputBytes;

      const baseName = normalizeRawOutputBaseName(entry.entryName);
      const inputKind = inferDecoderInputKind(entry.entryName);
      const outputFiles =
        inputKind === "raw"
          ? await decodeRawArchiveBinaryToCsvFiles(binaryBytes, baseName, outputMode)
          : await decodeThresholdEncodedBinaryToCsvFiles(binaryBytes, baseName, outputMode);

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
        reason:
          error instanceof Error ? error.message : "Unknown decode error.",
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

    const artifact = await buildDownloadArtifact(allOutputFiles, outputMode);
    triggerBlobDownload(artifact.blob, artifact.fileName);
    downloadedFileName = artifact.fileName;
  }

  emitProgress({
    phase: "done",
    completed: total,
    currentLabel: downloadedFileName
      ? `Downloaded ${downloadedFileName}`
      : "No decoded files were generated",
  });

  return {
    successes,
    failures,
    downloadedFileName,
  };
}