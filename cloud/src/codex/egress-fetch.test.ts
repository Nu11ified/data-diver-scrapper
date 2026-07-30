import { describe, expect, test } from "bun:test";

import { makeCodexFetch } from "./egress-fetch.ts";

const url = "https://chatgpt.com/backend-api/codex/responses";

describe("Codex container transport", () => {
  test("forwards only the Codex request and reconstructs its response", async () => {
    let sent:
      | {
          headers: Record<string, string>;
          bodyBase64: string;
        }
      | undefined;
    const codexFetch = makeCodexFetch(async (request) => {
      sent = request;
      return {
        status: 200,
        headers: { "content-type": "text/event-stream" },
        body: "data: [DONE]\n\n",
      };
    });

    const response = await codexFetch(url, {
      method: "POST",
      headers: {
        authorization: "Bearer tenant-token",
        "chatgpt-account-id": "tenant-account",
        "content-type": "application/json",
        session_id: "tenant-session",
      },
      body: '{"stream":true}',
    });

    expect(response.status).toBe(200);
    expect(response.headers.get("content-type")).toBe("text/event-stream");
    expect(await response.text()).toBe("data: [DONE]\n\n");
    expect(sent?.headers.authorization).toBe("Bearer tenant-token");
    expect(sent?.headers["chatgpt-account-id"]).toBe("tenant-account");
    expect(sent?.headers.session_id).toBe("tenant-session");
    expect(atob(sent?.bodyBase64 ?? "")).toBe('{"stream":true}');
  });

  test("rejects any destination other than the Codex Responses endpoint", async () => {
    const codexFetch = makeCodexFetch(async () => {
      throw new Error("must not send");
    });

    await expect(
      codexFetch("https://example.com/", {
        method: "POST",
        headers: {
          authorization: "Bearer tenant-token",
          "chatgpt-account-id": "tenant-account",
        },
      }),
    ).rejects.toThrow("rejected the destination");
  });

  test("requires both tenant authentication headers", async () => {
    const codexFetch = makeCodexFetch(async () => {
      throw new Error("must not send");
    });

    await expect(
      codexFetch(url, {
        method: "POST",
        headers: { authorization: "Bearer tenant-token" },
      }),
    ).rejects.toThrow("requires tenant authentication");
  });

  test("surfaces container RPC failures without request contents", async () => {
    const codexFetch = makeCodexFetch(async () =>
      Promise.reject({
        message: { code: "container_failed" },
        authorization: "Bearer secret",
      }),
    );

    await expect(
      codexFetch(url, {
        method: "POST",
        headers: {
          authorization: "Bearer tenant-token",
          "chatgpt-account-id": "tenant-account",
        },
      }),
    ).rejects.toThrow(
      'Codex egress failed: {"message":{"code":"container_failed"},"authorization":"[redacted]"}',
    );
  });
});
