import * as Data from "effect/Data";
import * as Effect from "effect/Effect";

export class EnvelopeError extends Data.TaggedError("EnvelopeError")<{
  readonly message: string;
}> {}

export interface SealedEnvelope {
  readonly ciphertext: Uint8Array<ArrayBuffer>;
  readonly iv: Uint8Array<ArrayBuffer>;
  readonly wrappedKey: Uint8Array<ArrayBuffer>;
}

const failWith =
  (context: string) =>
  (cause: unknown): EnvelopeError =>
    new EnvelopeError({
      message: `${context}: ${cause instanceof Error ? cause.message : String(cause)}`,
    });

export const importMasterKey = (base64: string): Effect.Effect<CryptoKey, EnvelopeError> =>
  Effect.tryPromise({
    try: async () => {
      const raw = Uint8Array.from(atob(base64), (c) => c.charCodeAt(0));
      if (raw.byteLength !== 32) {
        throw new Error(`master key must be 32 bytes, got ${raw.byteLength}`);
      }
      return crypto.subtle.importKey("raw", raw, { name: "AES-KW" }, false, [
        "wrapKey",
        "unwrapKey",
      ]);
    },
    catch: failWith("importMasterKey"),
  });

export const seal = (
  masterKey: CryptoKey,
  plaintext: string,
): Effect.Effect<SealedEnvelope, EnvelopeError> =>
  Effect.tryPromise({
    try: async () => {
      const dataKey = await crypto.subtle.generateKey({ name: "AES-GCM", length: 256 }, true, [
        "encrypt",
        "decrypt",
      ]);
      const iv = crypto.getRandomValues(new Uint8Array(12));
      const ciphertext = await crypto.subtle.encrypt(
        { name: "AES-GCM", iv },
        dataKey,
        new TextEncoder().encode(plaintext),
      );
      const wrappedKey = await crypto.subtle.wrapKey("raw", dataKey, masterKey, {
        name: "AES-KW",
      });
      return {
        ciphertext: new Uint8Array(ciphertext),
        iv,
        wrappedKey: new Uint8Array(wrappedKey),
      };
    },
    catch: failWith("seal"),
  });

export const open = (
  masterKey: CryptoKey,
  envelope: SealedEnvelope,
): Effect.Effect<string, EnvelopeError> =>
  Effect.tryPromise({
    try: async () => {
      const dataKey = await crypto.subtle.unwrapKey(
        "raw",
        envelope.wrappedKey,
        masterKey,
        { name: "AES-KW" },
        { name: "AES-GCM" },
        false,
        ["decrypt"],
      );
      const plaintext = await crypto.subtle.decrypt(
        { name: "AES-GCM", iv: envelope.iv },
        dataKey,
        envelope.ciphertext,
      );
      return new TextDecoder().decode(plaintext);
    },
    catch: failWith("open"),
  });
