import { describe, expect, test } from "bun:test";
import { RuntimeContext, type RuntimeContextInterface } from "alchemy";
import * as Effect from "effect/Effect";

import { DEFAULT_SPEC, compileSpec, type Graph, type TreeDoc } from "../decision/graph.ts";
import {
  makeThread,
  type Pending,
  type PropertyMatch,
  type ThreadStorage,
} from "./thread.ts";

const run = <A>(effect: Effect.Effect<A, never, RuntimeContextInterface>): Promise<A> =>
  Effect.runPromise(Effect.provide(effect, RuntimeContext.phantom));

const memoryStorage = (): ThreadStorage => {
  const cells = new Map<string, unknown>();
  return {
    get: <T>(key: string) => Effect.sync(() => cells.get(key) as T | undefined),
    put: (key: string, value: unknown) => Effect.sync(() => void cells.set(key, value)),
    delete: (key: string) => Effect.sync(() => void cells.delete(key)),
  };
};

const property = (
  overrides: Partial<PropertyMatch> & Pick<PropertyMatch, "propertyKey">,
): PropertyMatch => ({
  address: "1 Main St",
  owner: "DOE, JANE",
  mailing: "",
  phone: "",
  email: "",
  lifecycleState: "residential",
  owed: 40_000,
  assessed: 200_000,
  debtToValue: 0.2,
  violations: 0,
  sources: 2,
  signals: {},
  ...overrides,
});

const RESIDENTIAL = property({
  propertyKey: "norfolk_county_va|1",
  address: "10 Elm St",
  signals: { use_code: 1 },
});

const BUSINESS = property({
  propertyKey: "norfolk_county_va|2",
  address: "22 Commerce Way",
  owner: "ACME HOLDINGS LLC",
  signals: { use_code: 2 },
});

const CANDIDATES = [RESIDENTIAL, BUSINESS];

/// Matches only the business parcel; the saved default tree matches both.
const businessOnly: Graph = {
  entry: "business",
  nodes: [
    {
      kind: "condition",
      id: "business",
      field: "use_code",
      op: "gte",
      value: 2,
      onPass: "needs_approval",
      onFail: "discard",
    },
    { kind: "approval", id: "needs_approval", prompt: "Approve outreach", next: "match" },
    { kind: "action", id: "match", action: "match" },
    { kind: "action", id: "discard", action: "discard" },
  ],
};

const storedTree = async (storage: ThreadStorage): Promise<TreeDoc> => {
  const tree = await run(storage.get<TreeDoc>("tree"));
  if (tree === undefined) throw new Error("no tree stored");
  return tree;
};

const previewBusinessOnly = async (storage: ThreadStorage) => {
  const thread = makeThread(storage);
  await run(thread.handleMessage({ text: "review", candidates: CANDIDATES }));
  const baseline = await storedTree(storage);
  const preview = await run(
    thread.previewFilter({
      userText: "also show me business property today",
      lead: "Just for today:",
      graph: businessOnly,
      candidates: CANDIDATES,
    }),
  );
  return { thread, baseline, preview };
};

describe("previewFilter", () => {
  test("shows the overlay's matches and leaves the saved tree version alone", async () => {
    const storage = memoryStorage();
    const { baseline, preview } = await previewBusinessOnly(storage);

    expect(baseline.version).toBe(1);
    expect(preview.reply).toContain("22 Commerce Way");
    expect(preview.reply).not.toContain("10 Elm St");
    expect(preview.reply).toContain(`v${baseline.version}`);
    expect(preview.tree).toBeUndefined();

    const after = await storedTree(storage);
    expect(after.version).toBe(baseline.version);
    expect(after.graph).toEqual(baseline.graph);
  });

  test("persists no evaluations, because no stored tree version produced them", async () => {
    const storage = memoryStorage();
    const { preview } = await previewBusinessOnly(storage);
    expect(preview.evaluations).toEqual([]);
  });

  test("drilling into an overlay match credits the overlay, not the saved version", async () => {
    const storage = memoryStorage();
    const { thread, baseline } = await previewBusinessOnly(storage);

    const drill = await run(thread.handleMessage({ text: "1", candidates: CANDIDATES }));

    expect(drill.reply).toContain("22 Commerce Way");
    expect(drill.reply).toContain("use code 2");
    expect(drill.reply).toContain(
      `Why it matched (the one-off filter, not your saved criteria v${baseline.version}):`,
    );
    expect(drill.reply).not.toContain(`Why it matched (criteria v${baseline.version})`);
    expect(drill.evaluations).toEqual([]);

    const after = await storedTree(storage);
    expect(after.version).toBe(baseline.version);
    expect(after.graph).toEqual(baseline.graph);
  });

  test("drilling into a saved-criteria match still credits the saved version", async () => {
    const storage = memoryStorage();
    const thread = makeThread(storage);
    await run(thread.handleMessage({ text: "review", candidates: CANDIDATES }));
    const baseline = await storedTree(storage);

    const drill = await run(thread.handleMessage({ text: "1", candidates: CANDIDATES }));

    expect(drill.reply).toContain(`Why it matched (criteria v${baseline.version}):`);
    expect(drill.reply).not.toContain("one-off filter");
  });

  test("refuses a graph that fails validation and changes nothing", async () => {
    const storage = memoryStorage();
    const thread = makeThread(storage);
    await run(thread.handleMessage({ text: "review", candidates: CANDIDATES }));
    const before = await storedTree(storage);

    const outcome = await run(
      thread.previewFilter({
        userText: "only the ones with a pool",
        lead: "",
        graph: {
          entry: "pool",
          nodes: [
            {
              kind: "condition",
              id: "pool",
              field: "has_pool",
              op: "gte",
              value: 1,
              onPass: "match",
              onFail: "discard",
            },
            { kind: "action", id: "discard", action: "discard" },
          ],
        },
        candidates: CANDIDATES,
      }),
    );

    expect(outcome.reply).toContain('unknown signal "has_pool"');
    expect(outcome.reply).toContain('onPass -> missing "match"');
    expect(outcome.reply).toContain("no match action");
    expect(outcome.tree).toBeUndefined();

    const after = await storedTree(storage);
    expect(after.version).toBe(before.version);
    expect(after.graph).toEqual(before.graph);
    const pending = await run(storage.get<Pending>("pending"));
    expect(pending?.kind).not.toBe("remember_filter");
  });
});

describe("rememberFilter", () => {
  test("affirming promotes the previewed graph to a new tree version", async () => {
    const storage = memoryStorage();
    const { thread, baseline } = await previewBusinessOnly(storage);

    const outcome = await run(
      thread.rememberFilter({ userText: "yes", lead: "", remember: true }),
    );

    expect(outcome.tree?.version).toBe(baseline.version + 1);
    expect(outcome.tree?.graph).toEqual(businessOnly);

    const after = await storedTree(storage);
    expect(after.version).toBe(baseline.version + 1);
    expect(after.graph).toEqual(businessOnly);

    const review = await run(
      thread.handleMessage({ text: "review", candidates: CANDIDATES }),
    );
    expect(review.reply).toContain("22 Commerce Way");
    expect(review.reply).not.toContain("10 Elm St");
  });

  test("declining leaves the saved tree untouched", async () => {
    const storage = memoryStorage();
    const { thread, baseline } = await previewBusinessOnly(storage);

    const outcome = await run(
      thread.rememberFilter({ userText: "no", lead: "", remember: false }),
    );

    expect(outcome.tree).toBeUndefined();
    const after = await storedTree(storage);
    expect(after.version).toBe(baseline.version);
    expect(after.graph).toEqual(baseline.graph);

    const review = await run(
      thread.handleMessage({ text: "review", candidates: CANDIDATES }),
    );
    expect(review.reply).toContain("10 Elm St");
  });

  test("a bare YES after a preview promotes it through the same path", async () => {
    const storage = memoryStorage();
    const { thread, baseline } = await previewBusinessOnly(storage);

    await run(thread.handleMessage({ text: "yes", candidates: CANDIDATES }));

    const after = await storedTree(storage);
    expect(after.version).toBe(baseline.version + 1);
    expect(after.graph).toEqual(businessOnly);
  });

  test("a bare NO after a preview drops it", async () => {
    const storage = memoryStorage();
    const { thread, baseline } = await previewBusinessOnly(storage);

    await run(thread.handleMessage({ text: "no", candidates: CANDIDATES }));

    const after = await storedTree(storage);
    expect(after.version).toBe(baseline.version);
    expect(after.graph).toEqual(baseline.graph);
  });

  test("says so when nothing is waiting to be remembered", async () => {
    const storage = memoryStorage();
    const thread = makeThread(storage);
    const outcome = await run(
      thread.rememberFilter({ userText: "save that", lead: "", remember: true }),
    );

    expect(outcome.reply).toContain("no one-off filter waiting");
    const after = await storedTree(storage);
    expect(after.version).toBe(1);
    expect(after.graph).toEqual(compileSpec(DEFAULT_SPEC, after.name, 1).graph);
  });
});

const proposeBusinessOnly = async (storage: ThreadStorage) => {
  const thread = makeThread(storage);
  await run(thread.handleMessage({ text: "review", candidates: CANDIDATES }));
  const baseline = await storedTree(storage);
  const proposal = await run(
    thread.proposeTree({
      userText: "only businesses from now on",
      lead: "Switching to business-only criteria.",
      graph: businessOnly,
      candidates: CANDIDATES,
    }),
  );
  return { thread, baseline, proposal };
};

describe("proposeTree", () => {
  test("leaves the active tree untouched and previews the impact", async () => {
    const storage = memoryStorage();
    const { baseline, proposal } = await proposeBusinessOnly(storage);

    // The default tree (owed >= $10,000) matches both fixtures; the
    // business-only proposal drops the residential one and keeps the other.
    expect(proposal.reply).toContain(`active criteria v${baseline.version}`);
    expect(proposal.reply).toContain("1 remain qualified");
    expect(proposal.reply).toContain("1 removed: 10 Elm St");
    expect(proposal.reply).toContain("0 added");
    expect(proposal.reply).not.toContain("added: ");
    expect(proposal.reply).toContain(`v${baseline.version + 1}`);
    expect(proposal.tree).toBeUndefined();
    expect(proposal.evaluations).toEqual([]);

    const after = await storedTree(storage);
    expect(after.version).toBe(baseline.version);
    expect(after.graph).toEqual(baseline.graph);

    const pending = await run(storage.get<Pending>("pending"));
    expect(pending).toEqual({ kind: "approve_tree", graph: businessOnly });
  });

  test("counts an addition once the active tree is narrower than the proposal", async () => {
    const storage = memoryStorage();
    const { thread, baseline } = await proposeBusinessOnly(storage);
    await run(thread.handleMessage({ text: "approve", candidates: CANDIDATES }));
    const narrowed = await storedTree(storage);
    expect(narrowed.graph).toEqual(businessOnly);

    const proposal = await run(
      thread.proposeTree({
        userText: "open it back up to everyone",
        lead: "",
        graph: baseline.graph,
        candidates: CANDIDATES,
      }),
    );

    expect(proposal.reply).toContain("1 remain qualified");
    expect(proposal.reply).toContain("0 removed");
    expect(proposal.reply).toContain("1 added: 10 Elm St");
    expect(proposal.reply).toContain(`active criteria v${narrowed.version}`);
  });
});

describe("approve_tree pending", () => {
  test("APPROVE commits a new version and re-evaluates every candidate", async () => {
    const storage = memoryStorage();
    const { thread, baseline } = await proposeBusinessOnly(storage);

    const outcome = await run(
      thread.handleMessage({ text: "approve", candidates: CANDIDATES }),
    );

    expect(outcome.tree?.version).toBe(baseline.version + 1);
    expect(outcome.tree?.graph).toEqual(businessOnly);
    expect(outcome.reply).toContain(`v${baseline.version + 1}`);
    expect(outcome.evaluations).toHaveLength(CANDIDATES.length);
    expect(
      outcome.evaluations.find((e) => e.propertyKey === BUSINESS.propertyKey)?.outcome,
    ).toBe("match");
    expect(
      outcome.evaluations.find((e) => e.propertyKey === RESIDENTIAL.propertyKey)?.outcome,
    ).toBe("discard");

    const after = await storedTree(storage);
    expect(after.version).toBe(baseline.version + 1);
    expect(after.graph).toEqual(businessOnly);
    const pending = await run(storage.get<Pending>("pending"));
    expect(pending?.kind).toBe("idle");
  });

  test("REJECT discards the proposal and leaves the active tree untouched", async () => {
    const storage = memoryStorage();
    const { thread, baseline } = await proposeBusinessOnly(storage);

    const outcome = await run(
      thread.handleMessage({ text: "reject", candidates: CANDIDATES }),
    );

    expect(outcome.tree).toBeUndefined();
    expect(outcome.reply).toContain(`v${baseline.version}`);

    const after = await storedTree(storage);
    expect(after.version).toBe(baseline.version);
    expect(after.graph).toEqual(baseline.graph);
    const pending = await run(storage.get<Pending>("pending"));
    expect(pending?.kind).toBe("idle");
  });

  test("a new proposal supersedes a pending one still awaiting approval", async () => {
    const storage = memoryStorage();
    const { thread, baseline } = await proposeBusinessOnly(storage);

    await run(
      thread.proposeTree({
        userText: "actually keep everyone",
        lead: "",
        graph: baseline.graph,
        candidates: CANDIDATES,
      }),
    );
    const supersededPending = await run(storage.get<Pending>("pending"));
    expect(supersededPending).toEqual({ kind: "approve_tree", graph: baseline.graph });

    const outcome = await run(
      thread.handleMessage({ text: "approve", candidates: CANDIDATES }),
    );

    expect(outcome.tree?.graph).toEqual(baseline.graph);
    expect(outcome.tree?.version).toBe(baseline.version + 1);
  });
});
