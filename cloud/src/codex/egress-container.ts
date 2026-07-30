import * as Cloudflare from "alchemy/Cloudflare";
import type * as Effect from "effect/Effect";

export interface CodexEgressRequest {
  readonly headers: Record<string, string>;
  readonly bodyBase64: string;
  readonly compactOutput?: boolean;
}

export interface CodexEgressResponse {
  readonly status: number;
  readonly headers: Record<string, string>;
  readonly body: string;
}

export class CodexEgressContainer extends Cloudflare.Container<
  CodexEgressContainer,
  {
    request: (
      request: CodexEgressRequest,
    ) => Effect.Effect<CodexEgressResponse>;
  }
>()("CodexEgressContainer") {}
