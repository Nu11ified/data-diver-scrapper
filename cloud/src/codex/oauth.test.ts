import { describe, expect, test } from "bun:test";
import * as Effect from "effect/Effect";

import { CODEX_CLIENT_ID, authorizeUrl, challengeFor, identityFromIdToken, makeVerifier } from "./oauth.ts";

const fakeIdToken = (claims: Record<string, unknown>): string => {
  const encode = (value: Record<string, unknown>): string =>
    btoa(JSON.stringify(value)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
  return `${encode({ alg: "RS256" })}.${encode(claims)}.signature`;
};

describe("authorizeUrl", () => {
  test("targets auth.openai.com with PKCE and the codex client id", () => {
    const url = new URL(
      authorizeUrl({
        redirectUri: "https://worker.example/connect/callback",
        state: "tok_1",
        challenge: "chal",
      }),
    );
    expect(url.origin).toBe("https://auth.openai.com");
    expect(url.pathname).toBe("/oauth/authorize");
    expect(url.searchParams.get("client_id")).toBe(CODEX_CLIENT_ID);
    expect(url.searchParams.get("code_challenge_method")).toBe("S256");
    expect(url.searchParams.get("state")).toBe("tok_1");
    expect(url.searchParams.get("redirect_uri")).toBe("https://worker.example/connect/callback");
  });
});

describe("pkce", () => {
  test("challenge is the base64url sha256 of the verifier", async () => {
    const verifier = makeVerifier();
    const challenge = await Effect.runPromise(challengeFor(verifier));
    expect(challenge).toMatch(/^[A-Za-z0-9_-]{43}$/);
    expect(challenge).not.toBe(verifier);
    expect(await Effect.runPromise(challengeFor(verifier))).toBe(challenge);
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
