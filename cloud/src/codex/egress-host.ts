import * as Cloudflare from "alchemy/Cloudflare";
import * as Effect from "effect/Effect";

import {
  CodexEgressContainer,
  type CodexEgressRequest,
  type CodexEgressResponse,
} from "./egress-container.ts";

export class CodexEgressHost extends Cloudflare.DurableObject<
  CodexEgressHost,
  {
    readonly request: (
      request: CodexEgressRequest,
    ) => Effect.Effect<CodexEgressResponse>;
  }
>()("CodexEgressHost") {}

export const CodexEgressHostLive = CodexEgressHost.make(
  Effect.gen(function* () {
    const container = yield* CodexEgressContainer;
    return Effect.succeed({
      request: (request: CodexEgressRequest) => container.request(request),
    });
  }).pipe(
    Effect.provide(
      Cloudflare.Containers.layer(CodexEgressContainer, {
        enableInternet: true,
      }),
    ),
  ),
);
