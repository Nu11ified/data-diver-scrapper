import * as Data from "effect/Data";
import * as Effect from "effect/Effect";
import * as Option from "effect/Option";
import * as Schema from "effect/Schema";

export class CodexError extends Data.TaggedError("CodexError")<{
  readonly message: string;
  readonly status: number;
}> {}

/// Conversation is chatty and constant, so it runs on the cheaper, faster
/// model; hunting county records is the reasoning-heavy job and gets the
/// stronger one.
export const CHAT_MODEL = "gpt-5.6-luna";
export const RESEARCH_MODEL = "gpt-5.6-sol";
export const DEFAULT_CODEX_MODEL = CHAT_MODEL;
const CODEX_URL = "https://chatgpt.com/backend-api/codex/responses";

const DeltaEvent = Schema.Struct({
  type: Schema.Literal("response.output_text.delta"),
  delta: Schema.String,
});

const OutputTextPart = Schema.Struct({
  type: Schema.Literal("output_text"),
  text: Schema.String,
});

const MessageItem = Schema.Struct({
  type: Schema.Literal("message"),
  content: Schema.Array(Schema.Unknown),
});

const CompletedEvent = Schema.Struct({
  type: Schema.Literal("response.completed"),
  response: Schema.Struct({ output: Schema.Array(Schema.Unknown) }),
});

const decodeDelta = Schema.decodeUnknownOption(DeltaEvent);
const decodeCompleted = Schema.decodeUnknownOption(CompletedEvent);
const decodeMessage = Schema.decodeUnknownOption(MessageItem);
const decodePart = Schema.decodeUnknownOption(OutputTextPart);

export const extractOutputText = (sse: string): string => {
  const deltas: string[] = [];
  let completed = "";
  for (const line of sse.split("\n")) {
    if (!line.startsWith("data:")) continue;
    const payload = line.slice(5).trim();
    if (payload === "" || payload === "[DONE]") continue;
    let event: unknown;
    try {
      event = JSON.parse(payload);
    } catch {
      continue;
    }
    const delta = decodeDelta(event);
    if (Option.isSome(delta)) {
      deltas.push(delta.value.delta);
      continue;
    }
    const done = decodeCompleted(event);
    if (Option.isSome(done)) {
      completed = done.value.response.output
        .flatMap((item) => {
          const message = decodeMessage(item);
          if (Option.isNone(message)) return [];
          return message.value.content.flatMap((part) => {
            const text = decodePart(part);
            return Option.isSome(text) ? [text.value.text] : [];
          });
        })
        .join("");
    }
  }
  return completed !== "" ? completed : deltas.join("");
};

export interface CodexCall {
  readonly accessToken: string;
  readonly accountId: string;
  readonly instructions: string;
  readonly userText: string;
  readonly model?: string;
}

export const complete = (call: CodexCall): Effect.Effect<string, CodexError> =>
  Effect.tryPromise({
    try: async () => {
      const response = await fetch(CODEX_URL, {
        method: "POST",
        headers: {
          authorization: `Bearer ${call.accessToken}`,
          "chatgpt-account-id": call.accountId,
          "OpenAI-Beta": "responses=experimental",
          originator: "codex_cli_rs",
          session_id: crypto.randomUUID(),
          "content-type": "application/json",
          accept: "text/event-stream",
        },
        body: JSON.stringify({
          model: call.model ?? DEFAULT_CODEX_MODEL,
          instructions: call.instructions,
          input: [
            {
              type: "message",
              role: "user",
              content: [{ type: "input_text", text: call.userText }],
            },
          ],
          store: false,
          stream: true,
        }),
      });
      const body = await response.text();
      if (!response.ok) {
        throw new CodexError({
          message: `codex returned ${response.status}: ${body.slice(0, 300)}`,
          status: response.status,
        });
      }
      const text = extractOutputText(body);
      if (text === "") {
        throw new CodexError({
          message: `codex stream carried no output text: ${body.slice(0, 300)}`,
          status: response.status,
        });
      }
      return text;
    },
    catch: (cause): CodexError =>
      cause instanceof CodexError
        ? cause
        : new CodexError({
            message: cause instanceof Error ? cause.message : String(cause),
            status: 0,
          }),
  });
