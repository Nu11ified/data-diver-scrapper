import { describe, expect, test } from "bun:test";
import * as Effect from "effect/Effect";

import { EnvelopeError, importMasterKey, open, seal } from "./envelope.ts";

const randomMasterKeyBase64 = (): string =>
  btoa(String.fromCharCode(...crypto.getRandomValues(new Uint8Array(32))));

const run = <A>(effect: Effect.Effect<A, EnvelopeError>): Promise<A> =>
  Effect.runPromise(effect);

describe("envelope", () => {
  test("seal then open round-trips the plaintext", async () => {
    const master = await run(importMasterKey(randomMasterKeyBase64()));
    const sealed = await run(seal(master, `{"refresh_token":"rt-123"}`));
    expect(sealed.iv.byteLength).toBe(12);
    expect(sealed.ciphertext.byteLength).toBeGreaterThan(0);
    // AES-KW of a 32-byte key is 40 bytes
    expect(sealed.wrappedKey.byteLength).toBe(40);
    const opened = await run(open(master, sealed));
    expect(opened).toBe(`{"refresh_token":"rt-123"}`);
  });

  test("each seal uses a fresh data key and iv", async () => {
    const master = await run(importMasterKey(randomMasterKeyBase64()));
    const a = await run(seal(master, "same"));
    const b = await run(seal(master, "same"));
    expect(Buffer.from(a.iv).equals(Buffer.from(b.iv))).toBe(false);
    expect(Buffer.from(a.wrappedKey).equals(Buffer.from(b.wrappedKey))).toBe(false);
  });

  test("tampered ciphertext fails to open", async () => {
    const master = await run(importMasterKey(randomMasterKeyBase64()));
    const sealed = await run(seal(master, "secret"));
    const flipped = Uint8Array.from(sealed.ciphertext);
    const first = flipped[0];
    if (first === undefined) throw new Error("empty ciphertext");
    flipped[0] = first ^ 0xff;
    const result = await Effect.runPromiseExit(open(master, { ...sealed, ciphertext: flipped }));
    expect(result._tag).toBe("Failure");
  });

  test("the wrong master key cannot unwrap", async () => {
    const master = await run(importMasterKey(randomMasterKeyBase64()));
    const other = await run(importMasterKey(randomMasterKeyBase64()));
    const sealed = await run(seal(master, "secret"));
    const result = await Effect.runPromiseExit(open(other, sealed));
    expect(result._tag).toBe("Failure");
  });

  test("a short master key is rejected", async () => {
    const short = btoa(String.fromCharCode(...crypto.getRandomValues(new Uint8Array(16))));
    const result = await Effect.runPromiseExit(importMasterKey(short));
    expect(result._tag).toBe("Failure");
  });
});
