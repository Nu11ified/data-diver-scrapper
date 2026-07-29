import { describe, expect, test } from "bun:test";
import {
  fauxAssistantMessage,
  fauxProvider,
  fauxToolCall,
} from "@earendil-works/pi-ai";

import { DEFAULT_SPEC, compileSpec } from "../decision/graph.ts";
import { runPiScout } from "./pi.ts";
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
  test("turns a vague onboarding answer into a typed profile update", async () => {
    const faux = fauxProvider();
    faux.setResponses([
      fauxAssistantMessage(
        fauxToolCall("update_profile", {
          text: "I recommend requiring two independent county sources.",
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
      text: "I recommend requiring two independent county sources.",
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
});
