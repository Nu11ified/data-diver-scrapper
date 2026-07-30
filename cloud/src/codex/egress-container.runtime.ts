import * as Effect from "effect/Effect";
import * as HttpServerResponse from "effect/unstable/http/HttpServerResponse";

import { compactOutputStream } from "./client.ts";
import { CodexEgressContainer } from "./egress-container.ts";

const CODEX_URL = "https://chatgpt.com/backend-api/codex/responses";
const RESPONSE_HEADERS = new Set([
  "content-type",
  "openai-request-id",
  "request-id",
  "retry-after",
  "retry-after-ms",
  "x-request-id",
]);

const decodeBase64 = (value: string): Uint8Array => {
  const binary = atob(value);
  return Uint8Array.from(binary, (character) => character.charCodeAt(0));
};

export default CodexEgressContainer.make(
  {
    main: import.meta.url,
    instanceType: "lite",
    maxInstances: 3,
  },
  Effect.succeed(
    CodexEgressContainer.of({
      request: (request) =>
        Effect.promise(async () => {
          const requestBody = decodeBase64(request.bodyBase64);
          const response = await fetch(CODEX_URL, {
            method: "POST",
            headers: request.headers,
            body: requestBody,
            signal: AbortSignal.timeout(45_000),
          });
          const headers = Object.fromEntries(
            [...response.headers].filter(([name]) =>
              RESPONSE_HEADERS.has(name.toLowerCase()),
            ),
          );
          const body = await response.text();
          return {
            status: response.status,
            headers,
            body: request.compactOutput === true ? compactOutputStream(body) : body,
          };
        }),
      fetch: Effect.succeed(HttpServerResponse.text("ok")),
    }),
  ),
);
