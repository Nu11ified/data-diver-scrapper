import { neon } from "@neondatabase/serverless";

import { DEFAULT_SPEC, compileSpec } from "../src/decision/graph.ts";

if (process.env.DATABASE_URL === undefined) process.loadEnvFile(".env");

const databaseUrl = process.env.DATABASE_URL;
const secret = process.env.SENDBLUE_SECRET_KEY;
const origin =
  process.argv[2] ??
  process.env.PUBLIC_ORIGIN ??
  "https://datadiver-scraper-dev-manas-5riehtypkrdxb4fl.jrpnull.workers.dev";
if (databaseUrl === undefined || secret === undefined) {
  throw new Error("DATABASE_URL and SENDBLUE_SECRET_KEY are required");
}

const sql = neon(databaseUrl);
const tenants = await sql`
  select t.id, t.phone
  from tenants t
  join credentials c on c."tenantId" = t.id
  left join messages m on m."tenantId" = t.id
  group by t.id, t.phone
  order by max(m.at) desc nulls last
  limit 1
`;
const tenant = tenants[0];
if (tenant === undefined) throw new Error("no connected tenant is available");

const originalState = await sql`
  select key, value
  from conversation_state
  where "tenantId" = ${tenant.id}
`;
const tree = compileSpec(DEFAULT_SPEC, "acquisition", 1);
const at = "2026-07-30T12:00:00.000Z";

type ProbeResult = {
  readonly ok?: boolean;
  readonly brainDecision?: string;
  readonly brainReply?: string;
  readonly brainProfileUpdate?: {
    readonly markets?: readonly string[];
    readonly evidence?: string;
  };
  readonly scoutError?: string;
};

const replaceState = async (state: Readonly<Record<string, unknown>>) => {
  await sql`delete from conversation_state where "tenantId" = ${tenant.id}`;
  for (const [key, value] of Object.entries(state)) {
    await sql`
      insert into conversation_state ("tenantId", key, value, "updatedAt")
      values (
        ${tenant.id},
        ${key},
        ${JSON.stringify(value)}::jsonb,
        now()
      )
    `;
  }
};

const probe = async (
  text: string,
  state: Readonly<Record<string, unknown>>,
): Promise<ProbeResult> => {
  await replaceState(state);
  const response = await fetch(`${origin.replace(/\/+$/, "")}/sms`, {
    method: "POST",
    headers: {
      "content-type": "application/json",
      "x-datadiver-probe": secret,
    },
    body: JSON.stringify({
      from_number: tenant.phone,
      to_number: "+13472551483",
      content: text,
    }),
  });
  return await response.json() as ProbeResult;
};

const baseState = {
  tree,
  summary: "",
  pending: { kind: "idle" },
};

const scenarios = [
  {
    name: "multi-market intent",
    text: "Hey, I want to compare two counties at the same time.",
    state: baseState,
    valid: (result: ProbeResult) =>
      result.brainDecision === "reply" &&
      /which|name|send/i.test(result.brainReply ?? "") &&
      /two|both|count/i.test(result.brainReply ?? ""),
  },
  {
    name: "ambiguous market states",
    text: "Norfolk and Cincinnati.",
    state: {
      ...baseState,
      turns: [
        {
          role: "user",
          text: "I want to compare two counties at the same time.",
          at,
        },
        {
          role: "scout",
          text: "Absolutely. Which two counties or county-equivalent cities should I compare?",
          at,
        },
      ],
    },
    valid: (result: ProbeResult) =>
      result.brainDecision === "reply" &&
      /state/i.test(result.brainReply ?? ""),
  },
  {
    name: "state answer plus why question",
    text:
      "Norfolk is in Virginia and Cincinnati is in Ohio. Why do you need the states?",
    state: {
      ...baseState,
      turns: [
        { role: "user", text: "Norfolk and Cincinnati.", at },
        {
          role: "scout",
          text: "Which state belongs to each place?",
          at,
        },
      ],
    },
    valid: (result: ProbeResult) =>
      result.brainDecision === "update_profile" &&
      result.brainProfileUpdate?.markets?.join("|") ===
        "Norfolk, VA|Cincinnati, OH" &&
      /official|record|source|jurisdiction/i.test(result.brainReply ?? ""),
  },
  {
    name: "incidental state context",
    text: "I live in Virginia, but I invest nationwide.",
    state: {
      ...baseState,
      turns: [
        {
          role: "scout",
          text: "Which markets should I compare?",
          at,
        },
      ],
    },
    valid: (result: ProbeResult) =>
      result.brainDecision === "reply" &&
      result.brainProfileUpdate === undefined,
  },
  {
    name: "criteria answer plus why question",
    text:
      "Use open code violations with no debt floor. Why do you usually recommend two sources?",
    state: {
      ...baseState,
      county: "Norfolk, VA",
      markets: ["Norfolk, VA", "Cincinnati, OH"],
      onboarding: {
        markets: ["Norfolk, VA", "Cincinnati, OH"],
        county: "Norfolk, VA",
      },
      turns: [
        {
          role: "scout",
          text: "What should make a property distressed?",
          at,
        },
      ],
    },
    valid: (result: ProbeResult) =>
      result.brainDecision === "update_profile" &&
      result.brainProfileUpdate?.evidence === "open_violation" &&
      /independent|corrobor|confirm|error/i.test(result.brainReply ?? ""),
  },
  {
    name: "why does not resolve approval",
    text: "Why do I have to approve this search?",
    state: {
      ...baseState,
      county: "Norfolk, VA",
      markets: ["Norfolk, VA", "Cincinnati, OH"],
      onboarding: {
        markets: ["Norfolk, VA", "Cincinnati, OH"],
        county: "Norfolk, VA",
        minOwed: 0,
        minAssessed: 0,
        evidence: "open_violation",
        recencyAnswered: true,
        requireApproval: true,
      },
      pending: { kind: "approve_tree", graph: tree.graph },
    },
    valid: (result: ProbeResult) =>
      result.brainDecision === "reply" &&
      /saved|change|control|confirm|criteria/i.test(result.brainReply ?? ""),
  },
  {
    name: "reasoned natural approval",
    text: "Got it - because it changes my saved search. Go ahead and approve it.",
    state: {
      ...baseState,
      county: "Norfolk, VA",
      markets: ["Norfolk, VA", "Cincinnati, OH"],
      pending: { kind: "approve_tree", graph: tree.graph },
    },
    valid: (result: ProbeResult) =>
      result.brainDecision === "resolve_pending",
  },
] as const;

let failed = false;
try {
  for (const scenario of scenarios) {
    const result = await probe(scenario.text, scenario.state);
    const passed = result.ok === true && scenario.valid(result);
    failed ||= !passed;
    console.log(
      JSON.stringify({
        scenario: scenario.name,
        passed,
        decision: result.brainDecision,
        reply: result.brainReply,
        update: result.brainProfileUpdate,
        error: result.scoutError,
      }),
    );
  }
} finally {
  await sql`delete from conversation_state where "tenantId" = ${tenant.id}`;
  for (const row of originalState) {
    await sql`
      insert into conversation_state ("tenantId", key, value, "updatedAt")
      values (
        ${tenant.id},
        ${row.key},
        ${JSON.stringify(row.value)}::jsonb,
        now()
      )
    `;
  }
}

if (failed) process.exitCode = 1;
