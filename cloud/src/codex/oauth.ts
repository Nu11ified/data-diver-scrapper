import * as Data from "effect/Data";
import * as Effect from "effect/Effect";
import * as Schema from "effect/Schema";

export class OAuthError extends Data.TaggedError("OAuthError")<{
  readonly message: string;
}> {}

export const CODEX_CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann";
const AUTH_BASE = "https://auth.openai.com";
export const DEVICE_VERIFICATION_URL = `${AUTH_BASE}/codex/device`;
const DEVICE_REDIRECT_URI = `${AUTH_BASE}/deviceauth/callback`;
const DEVICE_API_BASE = `${AUTH_BASE}/api/accounts/deviceauth`;

type Fetcher = (
  input: string | URL | globalThis.Request,
  init?: RequestInit,
) => Promise<Response>;

const base64Url = (bytes: Uint8Array): string =>
  btoa(String.fromCharCode(...bytes))
    .replace(/\+/g, "-")
    .replace(/\//g, "_")
    .replace(/=+$/, "");

const challengeFor = (verifier: string): Effect.Effect<string> =>
  Effect.promise(async () => {
    const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(verifier));
    return base64Url(new Uint8Array(digest));
  });

const DeviceCodeResponse = Schema.Struct({
  device_auth_id: Schema.String,
  user_code: Schema.optional(Schema.String),
  usercode: Schema.optional(Schema.String),
  interval: Schema.Union([Schema.String, Schema.Number]),
});

const DeviceAuthorizationResponse = Schema.Struct({
  authorization_code: Schema.String,
  code_challenge: Schema.String,
  code_verifier: Schema.String,
});

export interface DeviceCode {
  readonly deviceAuthId: string;
  readonly userCode: string;
  readonly intervalSeconds: number;
}

export type DeviceCodePoll =
  | { readonly status: "pending" }
  | {
      readonly status: "authorized";
      readonly authorizationCode: string;
      readonly codeChallenge: string;
      readonly codeVerifier: string;
    };

const oauthError = (cause: unknown): OAuthError =>
  new OAuthError({
    message: cause instanceof Error ? cause.message : String(cause),
  });

export const requestDeviceCode = (
  fetcher: Fetcher = fetch,
): Effect.Effect<DeviceCode, OAuthError> =>
  Effect.tryPromise({
    try: async () => {
      const response = await fetcher(`${DEVICE_API_BASE}/usercode`, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ client_id: CODEX_CLIENT_ID }),
      });
      const body = await response.text();
      if (!response.ok) {
        throw new Error(`device code endpoint returned ${response.status}: ${body.slice(0, 300)}`);
      }
      const value = await Schema.decodeUnknownPromise(DeviceCodeResponse)(JSON.parse(body));
      const userCode = value.user_code ?? value.usercode ?? "";
      const intervalSeconds =
        typeof value.interval === "number" ? value.interval : Number.parseInt(value.interval, 10);
      if (userCode === "" || !Number.isFinite(intervalSeconds) || intervalSeconds < 1) {
        throw new Error("device code endpoint returned an invalid code or polling interval");
      }
      return {
        deviceAuthId: value.device_auth_id,
        userCode,
        intervalSeconds,
      };
    },
    catch: oauthError,
  });

export const pollDeviceCode = (
  deviceCode: Pick<DeviceCode, "deviceAuthId" | "userCode">,
  fetcher: Fetcher = fetch,
): Effect.Effect<DeviceCodePoll, OAuthError> =>
  Effect.tryPromise({
    try: async () => {
      const response = await fetcher(`${DEVICE_API_BASE}/token`, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({
          device_auth_id: deviceCode.deviceAuthId,
          user_code: deviceCode.userCode,
        }),
      });
      if (response.status === 403 || response.status === 404) {
        return { status: "pending" } as const;
      }
      const body = await response.text();
      if (!response.ok) {
        throw new Error(`device authorization returned ${response.status}: ${body.slice(0, 300)}`);
      }
      const value = await Schema.decodeUnknownPromise(DeviceAuthorizationResponse)(
        JSON.parse(body),
      );
      return {
        status: "authorized",
        authorizationCode: value.authorization_code,
        codeChallenge: value.code_challenge,
        codeVerifier: value.code_verifier,
      } as const;
    },
    catch: oauthError,
  });

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

const postToken = (
  form: URLSearchParams,
  fetcher: Fetcher = fetch,
): Effect.Effect<unknown, OAuthError> =>
  Effect.tryPromise({
    try: async () => {
      const response = await fetcher(`${AUTH_BASE}/oauth/token`, {
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

const exchangeCode = (
  request: ExchangeRequest,
  fetcher: Fetcher = fetch,
): Effect.Effect<TokenSet, OAuthError> =>
  postToken(
    new URLSearchParams({
      grant_type: "authorization_code",
      code: request.code,
      redirect_uri: request.redirectUri,
      client_id: CODEX_CLIENT_ID,
      code_verifier: request.verifier,
    }),
    fetcher,
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

export const exchangeDeviceCode = (
  authorized: Extract<DeviceCodePoll, { readonly status: "authorized" }>,
  fetcher: Fetcher = fetch,
): Effect.Effect<TokenSet, OAuthError> =>
  Effect.gen(function* () {
    const expectedChallenge = yield* challengeFor(authorized.codeVerifier);
    if (expectedChallenge !== authorized.codeChallenge) {
      return yield* new OAuthError({ message: "device authorization returned invalid PKCE data" });
    }
    return yield* exchangeCode(
      {
        code: authorized.authorizationCode,
        redirectUri: DEVICE_REDIRECT_URI,
        verifier: authorized.codeVerifier,
      },
      fetcher,
    );
  });

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

export const jwtExpiresAt = (token: string): number | undefined => {
  const payload = token.split(".")[1];
  if (payload === undefined) return undefined;
  try {
    const claims = JSON.parse(
      atob(payload.replace(/-/g, "+").replace(/_/g, "/")),
    ) as { readonly exp?: unknown };
    return typeof claims.exp === "number" && Number.isFinite(claims.exp)
      ? claims.exp * 1_000
      : undefined;
  } catch {
    return undefined;
  }
};

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
