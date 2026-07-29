import { describe, expect, test } from "bun:test";
import * as Effect from "effect/Effect";

import {
  CODEX_CLIENT_ID,
  DEVICE_VERIFICATION_URL,
  exchangeDeviceCode,
  identityFromIdToken,
  jwtExpiresAt,
  pollDeviceCode,
  requestDeviceCode,
} from "./oauth.ts";

const fakeIdToken = (claims: Record<string, unknown>): string => {
  const encode = (value: Record<string, unknown>): string =>
    btoa(JSON.stringify(value)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
  return `${encode({ alg: "RS256" })}.${encode(claims)}.signature`;
};

describe("device code login", () => {
  test("requests a device code for the Codex client", async () => {
    const calls: Array<{ readonly url: string; readonly init?: RequestInit }> = [];
    const code = await Effect.runPromise(
      requestDeviceCode(async (input, init) => {
        calls.push({ url: String(input), init });
        return Response.json({
          device_auth_id: "device_1",
          user_code: "ABCD-1234",
          interval: "5",
        });
      }),
    );

    expect(code).toEqual({
      deviceAuthId: "device_1",
      userCode: "ABCD-1234",
      intervalSeconds: 5,
    });
    expect(DEVICE_VERIFICATION_URL).toBe("https://auth.openai.com/codex/device");
    expect(calls[0]?.url).toBe(
      "https://auth.openai.com/api/accounts/deviceauth/usercode",
    );
    expect(JSON.parse(String(calls[0]?.init?.body))).toEqual({
      client_id: CODEX_CLIENT_ID,
    });
  });

  test("treats a forbidden poll as pending", async () => {
    const result = await Effect.runPromise(
      pollDeviceCode(
        { deviceAuthId: "device_1", userCode: "ABCD-1234" },
        async () => new Response("", { status: 403 }),
      ),
    );
    expect(result).toEqual({ status: "pending" });
  });

  test("exchanges an authorized device code with OpenAI's registered callback", async () => {
    const verifier = "device-verifier";
    const digest = await crypto.subtle.digest(
      "SHA-256",
      new TextEncoder().encode(verifier),
    );
    const challenge = btoa(String.fromCharCode(...new Uint8Array(digest)))
      .replace(/\+/g, "-")
      .replace(/\//g, "_")
      .replace(/=+$/, "");
    const bodies: string[] = [];
    const tokens = await Effect.runPromise(
      exchangeDeviceCode(
        {
          status: "authorized",
          authorizationCode: "authorization-code",
          codeChallenge: challenge,
          codeVerifier: verifier,
        },
        async (_input, init) => {
          bodies.push(String(init?.body));
          return Response.json({
            access_token: "access",
            refresh_token: "refresh",
            id_token: "id",
          });
        },
      ),
    );

    expect(tokens).toEqual({
      accessToken: "access",
      refreshToken: "refresh",
      idToken: "id",
    });
    const form = new URLSearchParams(bodies[0]);
    expect(form.get("redirect_uri")).toBe(
      "https://auth.openai.com/deviceauth/callback",
    );
    expect(form.get("code_verifier")).toBe(verifier);
  });
});

describe("identityFromIdToken", () => {
  test("reads the chatgpt account id and email", async () => {
    const identity = await Effect.runPromise(
      identityFromIdToken(
        fakeIdToken({
          email: "scout@example.com",
          "https://api.openai.com/auth": { chatgpt_account_id: "acct_123" },
        }),
      ),
    );
    expect(identity).toEqual({ accountId: "acct_123", email: "scout@example.com" });
  });

  test("fails when the account id claim is absent", async () => {
    const result = await Effect.runPromiseExit(
      identityFromIdToken(fakeIdToken({ email: "scout@example.com" })),
    );
    expect(result._tag).toBe("Failure");
  });

  test("fails on a token that is not a JWT", async () => {
    const result = await Effect.runPromiseExit(identityFromIdToken("garbage"));
    expect(result._tag).toBe("Failure");
  });
});

describe("jwtExpiresAt", () => {
  test("reads the access-token expiry", () => {
    expect(jwtExpiresAt(fakeIdToken({ exp: 1_800_000_000 }))).toBe(
      1_800_000_000_000,
    );
  });

  test("returns undefined for an opaque or malformed token", () => {
    expect(jwtExpiresAt("opaque-token")).toBeUndefined();
    expect(jwtExpiresAt("header.not-json.signature")).toBeUndefined();
  });
});
