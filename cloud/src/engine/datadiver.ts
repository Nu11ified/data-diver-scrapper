
import wasmModule from "./datadiver.wasm";
import createModule, { type EmscriptenModule } from "./datadiver.mjs";

export interface FieldMapping {
  readonly field: string;
  readonly source_label: string;
  readonly label_similarity: number;
  readonly value_pass_rate: number;
  readonly confidence: number;
  readonly reformatted: boolean;
}

export interface PropertyEvent {
  readonly id: string;
  readonly property_key: string;
  readonly kind: string;
  readonly event_date: string;
  readonly recorded_at: string;
  readonly source_id: string;
  readonly as_of: string;
  readonly run_id: string;
  readonly amount: number;
  readonly confidence: number;
  readonly details: Readonly<Record<string, string>>;
}

export interface ProcessedDocument {
  readonly format: string;
  readonly fingerprint: string;
  readonly classification: string;
  readonly class_confidence: number;
  readonly mapping: { readonly confidence: number; readonly fields: readonly FieldMapping[] };
  readonly records: number;
  readonly extraction_rate: number;
  readonly newest_record_date: string;
  readonly parse_ms: number;
  readonly classify_ms: number;
  readonly map_ms: number;
  readonly events: readonly PropertyEvent[];
}

export interface ProcessInput {
  readonly schemaJson: string;
  readonly classifierJson: string;
  readonly columnModelJson: string;
  readonly contentType: string;
  readonly body: string;
  readonly sourceId: string;
  readonly jurisdiction: string;
  readonly asOf: string;
  readonly runId: string;
  readonly nowIso: string;
}

export class EngineError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "EngineError";
  }
}

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const engine = (): Promise<EmscriptenModule> =>
  createModule({
    instantiateWasm: (imports, onSuccess) => {
      onSuccess(new WebAssembly.Instance(wasmModule, imports), wasmModule);
      return {};
    },
  }).catch((cause: unknown) => {
    throw new EngineError(`wasm startup failed: ${String(cause)}`);
  });

const call = async (name: string, args: readonly string[]): Promise<unknown> => {
  const mod = await engine();
  const pointers = args.map((arg) => mod.stringToNewUTF8(arg));
  try {
    const resultPointer = mod.ccall(
      name,
      "number",
      pointers.map(() => "number" as const),
      pointers,
    );
    const text = mod.UTF8ToString(resultPointer);
    mod.ccall("dd_free", null, ["number"], [resultPointer]);
    return JSON.parse(text) as unknown;
  } finally {
    for (const pointer of pointers) mod._free(pointer);
  }
};

const unwrap = (value: unknown, context: string): Record<string, unknown> => {
  if (!isRecord(value)) throw new EngineError(`${context}: engine returned a non-object`);
  if (value.ok !== true) {
    const message = typeof value.error === "string" ? value.error : "unknown engine error";
    throw new EngineError(`${context}: ${message}`);
  }
  return value;
};

export const engineVersion = async (): Promise<{ engine: string; abi: number }> => {
  const value = unwrap(await call("dd_version", []), "dd_version");
  if (typeof value.engine !== "string" || typeof value.abi !== "number") {
    throw new EngineError("dd_version: malformed response");
  }
  return { engine: value.engine, abi: value.abi };
};

export const processDocument = async (input: ProcessInput): Promise<ProcessedDocument> => {
  const value = unwrap(
    await call("dd_process_document", [
      input.schemaJson,
      input.classifierJson,
      input.columnModelJson,
      input.contentType,
      input.body,
      input.sourceId,
      input.jurisdiction,
      input.asOf,
      input.runId,
      input.nowIso,
    ]),
    `process ${input.sourceId}`,
  );
  if (
    typeof value.classification !== "string" ||
    typeof value.class_confidence !== "number" ||
    typeof value.extraction_rate !== "number" ||
    !Array.isArray(value.events) ||
    !isRecord(value.mapping)
  ) {
    throw new EngineError(`process ${input.sourceId}: malformed engine response`);
  }
  return value as unknown as ProcessedDocument;
};

export const compileCounty = async (
  schemaJson: string,
  events: readonly PropertyEvent[],
  trust: Readonly<Record<string, Readonly<Record<string, number>>>>,
  county: string,
): Promise<string> => {
  const mod = await engine();
  const args = [schemaJson, JSON.stringify(events), JSON.stringify(trust), county];
  const pointers = args.map((arg) => mod.stringToNewUTF8(arg));
  try {
    const resultPointer = mod.ccall(
      "dd_compile_county",
      "number",
      pointers.map(() => "number" as const),
      pointers,
    );
    const text = mod.UTF8ToString(resultPointer);
    mod.ccall("dd_free", null, ["number"], [resultPointer]);
    const parsed = JSON.parse(text) as unknown;
    if (isRecord(parsed) && parsed.ok === false) {
      const message = typeof parsed.error === "string" ? parsed.error : "unknown engine error";
      throw new EngineError(`compile ${county}: ${message}`);
    }
    return text;
  } finally {
    for (const pointer of pointers) mod._free(pointer);
  }
};
