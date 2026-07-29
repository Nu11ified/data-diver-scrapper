import * as Data from "effect/Data";
import * as Effect from "effect/Effect";
import * as Schema from "effect/Schema";

export class OAuthError extends Data.TaggedError("OAuthError")<{
  readonly message: string;
}> {}

// Public OAuth client id of the Codex CLI; the flow only works for this app.
export const CODEX_CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann";
const AUTH_BASE = "https://auth.openai.com";
const SCOPE = "openid profile email offline_access";

const base64Url = (bytes: Uint8Array): string =>
  btoa(String.fromCharCode(...bytes))
    .replace(/\+/g, "-")
    .replace(/\//g, "_")
    .replace(/=+$/, "");

export const makeVerifier = (): string => base64Url(crypto.getRandomValues(new Uint8Array(48)));

export const challengeFor = (verifier: string): Effect.Effect<string> =>
  Effect.promise(async () => {
    const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(verifier));
    return base64Url(new Uint8Array(digest));
  });

export interface AuthorizeRequest {
  readonly redirectUri: string;
  readonly state: string;
  readonly challenge: string;
}

export const authorizeUrl = (request: AuthorizeRequest): string => {
  const params = new URLSearchParams({
    response_type: "code",
    client_id: CODEX_CLIENT_ID,
    redirect_uri: request.redirectUri,
    scope: SCOPE,
    state: request.state,
    code_challenge: request.challenge,
    code_challenge_method: "S256",
    id_token_add_organizations: "true",
  });
  return `${AUTH_BASE}/oauth/authorize?${params.toString()}`;
};

const ExchangeResponse = Schema.Struct({
  access_token: Schema.String,
  refresh_token: Schema.String,
  id_token: Schema.String,
});

const RefreshResponse = Schema.Struct({
  access_token: Schema.String,
  refresh_token: Schema.optional(Schema.String),
  id_token: Schema.optional(Schema.String),
});

const postToken = (form: URLSearchParams): Effect.Effect<unknown, OAuthError> =>
  Effect.tryPromise({
    try: async () => {
      const response = await fetch(`${AUTH_BASE}/oauth/token`, {
        method: "POST",
        headers: { "content-type": "application/x-www-form-urlencoded" },
        body: form.toString(),
      });
      const body = await response.text();
      if (!response.ok) {
        throw new Error(`token endpoint returned ${response.status}: ${body.slice(0, 300)}`);
      }
      return JSON.parse(body) as unknown;
    },
    catch: (cause) =>
      new OAuthError({
        message: cause instanceof Error ? cause.message : String(cause),
      }),
  });

const badResponse = (issue: { readonly message: string }): OAuthError =>
  new OAuthError({ message: `unexpected token response: ${issue.message}` });

export interface TokenSet {
  readonly accessToken: string;
  readonly refreshToken: string;
  readonly idToken: string;
}

export interface ExchangeRequest {
  readonly code: string;
  readonly redirectUri: string;
  readonly verifier: string;
}

export const exchangeCode = (request: ExchangeRequest): Effect.Effect<TokenSet, OAuthError> =>
  postToken(
    new URLSearchParams({
      grant_type: "authorization_code",
      code: request.code,
      redirect_uri: request.redirectUri,
      client_id: CODEX_CLIENT_ID,
      code_verifier: request.verifier,
    }),
  ).pipe(
    Effect.flatMap((input) =>
      Schema.decodeUnknownEffect(ExchangeResponse)(input).pipe(Effect.mapError(badResponse)),
    ),
    Effect.map((tokens) => ({
      accessToken: tokens.access_token,
      refreshToken: tokens.refresh_token,
      idToken: tokens.id_token,
    })),
  );

export const refreshTokens = (
  refreshToken: string,
): Effect.Effect<{ readonly accessToken: string; readonly refreshToken: string }, OAuthError> =>
  postToken(
    new URLSearchParams({
      grant_type: "refresh_token",
      refresh_token: refreshToken,
      client_id: CODEX_CLIENT_ID,
      scope: "openid profile email",
    }),
  ).pipe(
    Effect.flatMap((input) =>
      Schema.decodeUnknownEffect(RefreshResponse)(input).pipe(Effect.mapError(badResponse)),
    ),
    Effect.map((tokens) => ({
      accessToken: tokens.access_token,
      refreshToken: tokens.refresh_token ?? refreshToken,
    })),
  );

const IdTokenClaims = Schema.Struct({
  email: Schema.optional(Schema.String),
  "https://api.openai.com/auth": Schema.optional(
    Schema.Struct({
      chatgpt_account_id: Schema.optional(Schema.String),
    }),
  ),
});

export interface CodexIdentity {
  readonly accountId: string;
  readonly email: string;
}

export const identityFromIdToken = (idToken: string): Effect.Effect<CodexIdentity, OAuthError> =>
  Effect.gen(function* () {
    const payload = idToken.split(".")[1];
    if (payload === undefined) {
      return yield* new OAuthError({ message: "id_token is not a JWT" });
    }
    const decoded = yield* Effect.try({
      try: () =>
        JSON.parse(
          atob(payload.replace(/-/g, "+").replace(/_/g, "/")),
        ) as unknown,
      catch: () => new OAuthError({ message: "id_token payload is not valid JSON" }),
    });
    const claims = yield* Schema.decodeUnknownEffect(IdTokenClaims)(decoded).pipe(
      Effect.mapError(
        (issue) => new OAuthError({ message: `unexpected id_token claims: ${issue.message}` }),
      ),
    );
    const accountId = claims["https://api.openai.com/auth"]?.chatgpt_account_id;
    if (accountId === undefined) {
      return yield* new OAuthError({
        message: "id_token carries no chatgpt_account_id; the account cannot call Codex",
      });
    }
    return { accountId, email: claims.email ?? "" };
  });

export const CredentialPayload = Schema.Struct({
  provider: Schema.Literal("codex"),
  accountId: Schema.String,
  email: Schema.String,
  accessToken: Schema.String,
  refreshToken: Schema.String,
  idToken: Schema.String,
  obtainedAt: Schema.String,
});
export type CredentialPayload = (typeof CredentialPayload)["Type"];
