import type {
  CodexEgressRequest,
  CodexEgressResponse,
} from "./egress-container.ts";

const CODEX_URL = "https://chatgpt.com/backend-api/codex/responses";
const COMPACT_OUTPUT_HEADER = "x-datadiver-compact-output";
const REQUEST_HEADERS = new Set([
  "accept",
  "authorization",
  "chatgpt-account-id",
  "content-encoding",
  "content-type",
  "openai-beta",
  "originator",
  "session-id",
  "session_id",
  "user-agent",
  "x-client-request-id",
  COMPACT_OUTPUT_HEADER,
]);

export type CodexEgressSend = (
  request: CodexEgressRequest,
) => Promise<CodexEgressResponse>;

export type HttpFetch = (
  input: string | URL | Request,
  init?: RequestInit,
) => Promise<Response>;

const encodeBase64 = (buffer: ArrayBuffer): string => {
  const bytes = new Uint8Array(buffer);
  let binary = "";
  for (let offset = 0; offset < bytes.length; offset += 0x8000) {
    binary += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000));
  }
  return btoa(binary);
};

const describeFailure = (cause: unknown): string => {
  if (cause instanceof Error) return cause.message;
  if (typeof cause !== "object" || cause === null) return String(cause);
  const record = cause as Record<string, unknown>;
  const message = record.message;
  if (typeof message === "string") return message;
  try {
    return JSON.stringify(cause, (key, value) =>
      /authorization|body|header|request|token/i.test(key)
        ? "[redacted]"
        : value,
    );
  } catch {
    return "unknown container RPC failure";
  }
};

export const makeCodexFetch =
  (send: CodexEgressSend): HttpFetch =>
  async (input, init) => {
    const request =
      typeof input === "string" || input instanceof URL
        ? new Request(String(input), init)
        : new Request(input, init);
    if (request.url !== CODEX_URL || request.method !== "POST") {
      throw new Error("Codex egress rejected the destination");
    }

    const headers: Record<string, string> = {};
    let compactOutput = false;
    for (const [name, value] of request.headers) {
      const normalized = name.toLowerCase();
      if (!REQUEST_HEADERS.has(normalized)) {
        throw new Error(`Codex egress rejected header ${normalized}`);
      }
      if (normalized === COMPACT_OUTPUT_HEADER) {
        compactOutput = value === "1";
        continue;
      }
      headers[normalized] = value;
    }
    if (
      !headers.authorization?.startsWith("Bearer ") ||
      headers["chatgpt-account-id"] === undefined
    ) {
      throw new Error("Codex egress requires tenant authentication");
    }

    const response = await send({
      headers,
      bodyBase64: encodeBase64(await request.arrayBuffer()),
      ...(compactOutput ? { compactOutput: true } : {}),
    }).catch((cause: unknown) => {
      throw new Error(`Codex egress failed: ${describeFailure(cause)}`);
    });
    return new Response(response.body, {
      status: response.status,
      headers: response.headers,
    });
  };
