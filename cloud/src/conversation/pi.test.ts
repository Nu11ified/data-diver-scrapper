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
  test("accepts a natural confirmation without teaching a keyword", async () => {
    const faux = fauxProvider();
    faux.setResponses([
      fauxAssistantMessage(
        fauxToolCall("resolve_pending", {
          text: "That search looks right.",
          approved: true,
        }),
        { stopReason: "toolUse" },
      ),
    ]);

    const result = await runPiScout(
      {
        accessToken: "test-token",
        sessionId: "tenant-natural-approval",
        userText: "Looks right, save it",
        context: {
          ...context({
            county: "Norfolk, VA",
            minOwed: 10_000,
            minAssessed: 0,
            evidence: "multiple_sources",
            recencyAnswered: true,
            requireApproval: true,
          }),
          pending: "approve_tree",
        },
      },
      {
        model: faux.getModel(),
        streamFn: faux.provider.streamSimple.bind(faux.provider),
      },
    );

    expect(result.decision).toEqual({
      kind: "resolve_pending",
      text: "That search looks right.",
      approved: true,
    });
  });

  test("routes outreach wording changes without dropping the selected property", async () => {
    const faux = fauxProvider();
    faux.setResponses([
      fauxAssistantMessage(
        fauxToolCall("revise_outreach", {
          text: "I will make the draft warmer and keep the property details.",
          instruction: "Use a warmer, less formal tone.",
        }),
        { stopReason: "toolUse" },
      ),
    ]);

    const result = await runPiScout(
      {
        accessToken: "test-token",
        sessionId: "tenant-outreach-revision",
        userText: "Make it warmer",
        context: {
          ...context(),
          configured: true,
          pending: "approve_outreach",
        },
      },
      {
        model: faux.getModel(),
        streamFn: faux.provider.streamSimple.bind(faux.provider),
      },
    );

    expect(result.decision).toEqual({
      kind: "revise_outreach",
      text: "I will make the draft warmer and keep the property details.",
      instruction: "Use a warmer, less formal tone.",
    });
  });

  test("treats a correction to a completed proposal as an update, not a restart", async () => {
    const faux = fauxProvider();
    faux.setResponses([
      fauxAssistantMessage(
        fauxToolCall("update_profile", {
          text: "Changed the minimum owed to $20,000.",
          minOwed: 20_000,
        }),
        { stopReason: "toolUse" },
      ),
    ]);

    const result = await runPiScout(
      {
        accessToken: "test-token",
        sessionId: "tenant-correction",
        userText: "No, make the minimum $20,000",
        context: {
          ...context({
            county: "Norfolk, VA",
            minOwed: 10_000,
            minAssessed: 0,
            evidence: "multiple_sources",
            recencyAnswered: true,
            requireApproval: true,
          }),
          pending: "approve_tree",
        },
      },
      {
        model: faux.getModel(),
        streamFn: faux.provider.streamSimple.bind(faux.provider),
      },
    );

    expect(result.decision).toEqual({
      kind: "update_profile",
      text: "Changed the minimum owed to $20,000.",
      update: { minOwed: 20_000 },
    });
  });

  test("captures a natural search request in one update and defaults only omissions", async () => {
    const faux = fauxProvider();
    faux.setResponses([
      fauxAssistantMessage(
        fauxToolCall("update_profile", {
          text: "I captured your Norfolk search and multi-source requirement.",
          county: "Norfolk, VA",
          minOwed: 10_000,
          evidence: "multiple_sources",
          completeWithDefaults: true,
        }),
        { stopReason: "toolUse" },
      ),
    ]);

    const result = await runPiScout(
      {
        accessToken: "test-token",
        sessionId: "tenant-natural-search",
        userText:
          "I'm looking for distressed properties in Norfolk, VA with at least " +
          "$10,000 owed and evidence from multiple county sources.",
        context: context(),
      },
      {
        model: faux.getModel(),
        streamFn: faux.provider.streamSimple.bind(faux.provider),
      },
    );

    expect(result.decision).toEqual({
      kind: "update_profile",
      text: "I captured your Norfolk search and multi-source requirement.",
      update: {
        county: "Norfolk, VA",
        minOwed: 10_000,
        evidence: "multiple_sources",
      },
      completeWithDefaults: true,
    });
  });

  test("accepts multiple out-of-order preferences before the market is complete", async () => {
    const faux = fauxProvider();
    faux.setResponses([
      fauxAssistantMessage(
        fauxToolCall("update_profile", {
          text:
            "I captured the $10,000 debt floor and multi-source evidence.\n\n" +
            "Which state is Norfolk in, and should I use safe defaults for everything else?",
          minOwed: 10_000,
          evidence: "multiple_sources",
        }),
        { stopReason: "toolUse" },
      ),
    ]);

    const result = await runPiScout(
      {
        accessToken: "test-token",
        sessionId: "tenant-ambiguous-market",
        userText: "Norfolk, $10k owed, multiple sources",
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
        "I captured the $10,000 debt floor and multi-source evidence.\n\n" +
        "Which state is Norfolk in, and should I use safe defaults for everything else?",
      update: { minOwed: 10_000, evidence: "multiple_sources" },
    });
  });

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

  test("accepts a valid profile update without enforcing reply formatting", async () => {
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
        "Denton, TX is set as your market. What minimum delinquent amount should qualify?",
      update: { county: "Denton, TX" },
    });
  });

  test("stores an explicit zero preference even when the model routes it as a reply", async () => {
    const faux = fauxProvider();
    faux.setResponses([
      fauxAssistantMessage(
        fauxToolCall("reply", {
          text:
            "Norfolk, VA is set, with no assessed-value minimum. What other preferences should I use?",
        }),
        { stopReason: "toolUse" },
      ),
    ]);

    const result = await runPiScout(
      {
        accessToken: "test-token",
        sessionId: "tenant-explicit-zero",
        userText: "No assessed-value minimum.",
        context: context({ county: "Norfolk, VA" }),
      },
      {
        model: faux.getModel(),
        streamFn: faux.provider.streamSimple.bind(faux.provider),
      },
    );

    expect(result.decision).toEqual({
      kind: "update_profile",
      text:
        "Norfolk, VA is set, with no assessed-value minimum. What other preferences should I use?",
      update: { minAssessed: 0 },
    });
  });

  test("stores clear out-of-order criteria even when the model routes them as a reply", async () => {
    const faux = fauxProvider();
    faux.setResponses([
      fauxAssistantMessage(
        fauxToolCall("reply", {
          text:
            "Norfolk is set with a $25,000 minimum owed, any event age, and approval required.",
        }),
        { stopReason: "toolUse" },
      ),
    ]);

    const result = await runPiScout(
      {
        accessToken: "test-token",
        sessionId: "tenant-out-of-order-fallback",
        userText:
          "Keep Norfolk, make the minimum owed $25,000, allow any event age, and require my approval before outreach.",
        context: context({ county: "Norfolk, VA" }),
      },
      {
        model: faux.getModel(),
        streamFn: faux.provider.streamSimple.bind(faux.provider),
      },
    );

    expect(result.decision).toEqual({
      kind: "update_profile",
      text:
        "Norfolk is set with a $25,000 minimum owed, any event age, and approval required.",
      update: {
        minOwed: 25_000,
        anyEventAge: true,
        requireApproval: true,
      },
    });
  });

  test("does not let a correction silently default the rest of onboarding", async () => {
    const faux = fauxProvider();
    faux.setResponses([
      fauxAssistantMessage(
        fauxToolCall("update_profile", {
          text: "Changed the minimum owed to $20,000. Ready to continue?",
          minOwed: 20_000,
          completeWithDefaults: true,
        }),
        { stopReason: "toolUse" },
      ),
    ]);

    const result = await runPiScout(
      {
        accessToken: "test-token",
        sessionId: "tenant-correction-defaults",
        userText: "Actually, make the minimum owed $20,000 instead.",
        context: context({ county: "Norfolk, VA" }),
      },
      {
        model: faux.getModel(),
        streamFn: faux.provider.streamSimple.bind(faux.provider),
      },
    );

    expect(result.decision).toEqual({
      kind: "update_profile",
      text: "Changed the minimum owed to $20,000. Ready to continue?",
      update: { minOwed: 20_000 },
    });
  });

  test("does not let a state answer plus why question silently default the buy box", async () => {
    const faux = fauxProvider();
    faux.setResponses([
      fauxAssistantMessage(
        fauxToolCall("update_profile", {
          text:
            "Norfolk, VA and Cincinnati, OH are captured. States identify the correct official records. What should define your buy box?",
          markets: ["Norfolk, VA", "Cincinnati, OH"],
          completeWithDefaults: true,
        }),
        { stopReason: "toolUse" },
      ),
    ]);

    const result = await runPiScout(
      {
        accessToken: "test-token",
        sessionId: "tenant-state-why-no-defaults",
        userText:
          "Norfolk is in Virginia and Cincinnati is in Ohio. Why do you need the states?",
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
        "Norfolk, VA and Cincinnati, OH are captured. States identify the correct official records. What should define your buy box?",
      update: { markets: ["Norfolk, VA", "Cincinnati, OH"] },
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

  test("captures two resolved states while answering a why question", async () => {
    const faux = fauxProvider();
    faux.setResponses([
      fauxAssistantMessage(
        fauxToolCall("update_profile", {
          text:
            "The states disambiguate the official record systems. I have Norfolk, VA and Cincinnati, OH. What distress signal should define the search?",
          markets: ["Norfolk, VA", "Cincinnati, OH"],
        }),
        { stopReason: "toolUse" },
      ),
    ]);

    const result = await runPiScout(
      {
        accessToken: "test-token",
        sessionId: "tenant-two-markets",
        userText:
          "Norfolk is in Virginia and Cincinnati is in Ohio. Why do you need the states?",
        context: {
          ...context(),
          recentTurns: [
            {
              role: "user",
              text: "Norfolk and Cincinnati",
              at: "2026-07-29T12:00:00.000Z",
            },
            {
              role: "scout",
              text: "Which state goes with each place?",
              at: "2026-07-29T12:00:01.000Z",
            },
          ],
        },
      },
      {
        model: faux.getModel(),
        streamFn: faux.provider.streamSimple.bind(faux.provider),
      },
    );

    expect(result.decision).toEqual({
      kind: "update_profile",
      text:
        "The states disambiguate the official record systems. I have Norfolk, VA and Cincinnati, OH. What distress signal should define the search?",
      update: { markets: ["Norfolk, VA", "Cincinnati, OH"] },
    });
  });

  test("does not treat an incidental state mention as a market update", async () => {
    const faux = fauxProvider();
    faux.setResponses([
      fauxAssistantMessage(
        fauxToolCall("reply", {
          text:
            "Your location does not change the search. Which markets should I compare?",
        }),
        { stopReason: "toolUse" },
      ),
    ]);

    const result = await runPiScout(
      {
        accessToken: "test-token",
        sessionId: "tenant-incidental-state",
        userText: "I live in Virginia, but I invest nationwide.",
        context: context(),
      },
      {
        model: faux.getModel(),
        streamFn: faux.provider.streamSimple.bind(faux.provider),
      },
    );

    expect(result.decision).toEqual({
      kind: "reply",
      text: "Your location does not change the search. Which markets should I compare?",
    });
  });

  test("recovers an empty onboarding update as a natural question", async () => {
    const faux = fauxProvider();
    faux.setResponses([
      fauxAssistantMessage(
        fauxToolCall("update_profile", {
          text: "Absolutely. Which two markets should I compare?",
        }),
        { stopReason: "toolUse" },
      ),
    ]);

    const result = await runPiScout(
      {
        accessToken: "test-token",
        sessionId: "tenant-market-intent",
        userText: "I want to compare two counties at the same time.",
        context: context(),
      },
      {
        model: faux.getModel(),
        streamFn: faux.provider.streamSimple.bind(faux.provider),
      },
    );

    expect(result.decision).toEqual({
      kind: "reply",
      text: "Absolutely. Which two markets should I compare?",
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
    expect(
      legacyProfileText(
        { markets: ["Norfolk, VA", "Cincinnati, OH"] },
        "Both",
      ),
    ).toBe("Norfolk, VA and Cincinnati, OH");
  });
});
