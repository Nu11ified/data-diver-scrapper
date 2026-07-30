import { describe, expect, test } from "bun:test";
import {
  fauxAssistantMessage,
  fauxProvider,
  fauxToolCall,
} from "@earendil-works/pi-ai";

import { DEFAULT_SPEC, compileSpec } from "../decision/graph.ts";
import { runPiScout } from "./pi.ts";
import { legacyProfileText } from "./profile.ts";
import type { ScoutContext } from "./scout.ts";

const context = (profile: ScoutContext["profile"] = {}): ScoutContext => ({
  tree: compileSpec(DEFAULT_SPEC, "acquisition", 1),
  configured: false,
  summary: "",
  recentTurns: [
    {
      role: "scout",
      text: "4/6 — What extra evidence should a lead have?",
      at: "2026-07-29T12:00:00.000Z",
    },
  ],
  county: "Dallas, TX",
  candidateCount: 0,
  qualifiedCount: 0,
  extraSignals: [],
  profile,
});

describe("Pi conversation agent", () => {
  test("retries an invalid market normalization and preserves the supplied state", async () => {
    const faux = fauxProvider();
    faux.setResponses([
      fauxAssistantMessage(
        fauxToolCall("update_profile", {
          text: "Denton is set.",
          county: "Denton",
        }),
        { stopReason: "toolUse" },
      ),
      fauxAssistantMessage(
        fauxToolCall("update_profile", {
          text:
            "Denton, TX is set as your market.\n\n" +
            "What minimum recorded taxes or liens makes a property worth reviewing?",
          county: "Denton, TX",
        }),
        { stopReason: "toolUse" },
      ),
    ]);

    const result = await runPiScout(
      {
        accessToken: "test-token",
        sessionId: "tenant-market",
        userText: "Denton, TX",
        context: context(),
      },
      {
        model: faux.getModel(),
        streamFn: faux.provider.streamSimple.bind(faux.provider),
      },
    );

    expect(result.decision).toEqual({
      kind: "update_profile",
      text:
        "Denton, TX is set as your market.\n\n" +
        "What minimum recorded taxes or liens makes a property worth reviewing?",
      update: { county: "Denton, TX" },
    });
  });

  test("retries a flat onboarding response until it is readable as an SMS", async () => {
    const faux = fauxProvider();
    faux.setResponses([
      fauxAssistantMessage(
        fauxToolCall("update_profile", {
          text:
            "Denton, TX is set as your market. What minimum delinquent amount should qualify?",
          county: "Denton, TX",
        }),
        { stopReason: "toolUse" },
      ),
      fauxAssistantMessage(
        fauxToolCall("update_profile", {
          text:
            "Denton, TX is set as your market.\n\n" +
            "What minimum delinquent amount should qualify?",
          county: "Denton, TX",
        }),
        { stopReason: "toolUse" },
      ),
    ]);

    const result = await runPiScout(
      {
        accessToken: "test-token",
        sessionId: "tenant-sms-shape",
        userText: "Denton, TX",
        context: context(),
      },
      {
        model: faux.getModel(),
        streamFn: faux.provider.streamSimple.bind(faux.provider),
      },
    );

    expect(result.decision).toEqual({
      kind: "update_profile",
      text:
        "Denton, TX is set as your market.\n\n" +
        "What minimum delinquent amount should qualify?",
      update: { county: "Denton, TX" },
    });
  });

  test("turns a vague onboarding answer into a typed profile update", async () => {
    const faux = fauxProvider();
    faux.setResponses([
      fauxAssistantMessage(
        fauxToolCall("update_profile", {
          text:
            "I recommend requiring two independent county sources.\n\n" +
            "Should older records remain eligible?",
          evidence: "multiple_sources",
        }),
        { stopReason: "toolUse" },
      ),
    ]);

    const result = await runPiScout(
      {
        accessToken: "test-token",
        sessionId: "tenant-1",
        userText: "Whatever works",
        context: context({
          county: "Dallas, TX",
          minOwed: 20_000,
          minAssessed: 80_000,
        }),
      },
      {
        model: faux.getModel(),
        streamFn: faux.provider.streamSimple.bind(faux.provider),
      },
    );

    expect(result.decision).toEqual({
      kind: "update_profile",
      text:
        "I recommend requiring two independent county sources.\n\n" +
        "Should older records remain eligible?",
      update: { evidence: "multiple_sources" },
    });
  });

  test("keeps ordinary conversation as a natural reply", async () => {
    const faux = fauxProvider();
    faux.setResponses([
      fauxAssistantMessage("We can pause here. I will keep your answers for when you return."),
    ]);

    const result = await runPiScout(
      {
        accessToken: "test-token",
        sessionId: "tenant-2",
        userText: "I need to think about that",
        context: context(),
      },
      {
        model: faux.getModel(),
        streamFn: faux.provider.streamSimple.bind(faux.provider),
      },
    );

    expect(result.decision).toEqual({
      kind: "reply",
      text: "We can pause here. I will keep your answers for when you return.",
    });
  });

  test("never reuses an old assistant message when a turn has no new text", async () => {
    const faux = fauxProvider();
    faux.setResponses([
      fauxAssistantMessage([], {
        stopReason: "stop",
      }),
    ]);

    await expect(
      runPiScout(
        {
          accessToken: "test-token",
          sessionId: "tenant-3",
          userText: "Whatever works",
          context: context(),
        },
        {
          model: faux.getModel(),
          streamFn: faux.provider.streamSimple.bind(faux.provider),
        },
      ),
    ).rejects.toThrow("Pi agent ended without an action: user -> assistant(stop)[]");
  });

  test("surfaces provider errors instead of treating them as conversation", async () => {
    const faux = fauxProvider();
    faux.setResponses([
      fauxAssistantMessage([], {
        stopReason: "error",
        errorMessage: "provider rejected the request",
      }),
    ]);

    await expect(
      runPiScout(
        {
          accessToken: "test-token",
          sessionId: "tenant-4",
          userText: "Whatever works",
          context: context(),
        },
        {
          model: faux.getModel(),
          streamFn: faux.provider.streamSimple.bind(faux.provider),
        },
      ),
    ).rejects.toThrow("Pi agent failed: provider rejected the request");
  });
});

describe("Durable Object rollout compatibility", () => {
  test("converts typed profile choices into input understood by the previous object", () => {
    expect(
      legacyProfileText({ evidence: "multiple_sources" }, "Whatever works"),
    ).toBe("multiple sources");
    expect(legacyProfileText({ anyEventAge: true }, "You decide")).toBe("any");
    expect(legacyProfileText({ requireApproval: true }, "Not sure")).toBe("yes");
  });
});
