import * as Option from "effect/Option";
import * as Schema from "effect/Schema";

import { Graph, SIGNAL_CATALOG, type TreeDoc } from "../decision/graph.ts";

export const SourceCandidate = Schema.Struct({
  id: Schema.String,
  name: Schema.String,
  url: Schema.String,
});
export type SourceCandidate = (typeof SourceCandidate)["Type"];

export const ScoutDecision = Schema.Union([
  Schema.Struct({ kind: Schema.Literal("reply"), text: Schema.String }),
  Schema.Struct({
    kind: Schema.Literal("set_tree"),
    text: Schema.String,
    graph: Graph,
  }),
  Schema.Struct({
    kind: Schema.Literal("discover"),
    text: Schema.String,
    jurisdiction: Schema.String,
    candidates: Schema.Array(SourceCandidate),
  }),
]);
export type ScoutDecision = (typeof ScoutDecision)["Type"];

const decodeDecision = Schema.decodeUnknownOption(ScoutDecision);

export const parseScoutDecision = (raw: string): ScoutDecision => {
  const stripped = raw
    .trim()
    .replace(/^```(?:json)?\s*/, "")
    .replace(/\s*```$/, "");
  const start = stripped.indexOf("{");
  const end = stripped.lastIndexOf("}");
  if (start !== -1 && end > start) {
    try {
      const decoded = decodeDecision(JSON.parse(stripped.slice(start, end + 1)));
      if (Option.isSome(decoded)) return decoded.value;
    } catch {
      // fall through to the plain-reply fallback
    }
  }
  return { kind: "reply", text: raw.trim() };
};

export interface ScoutContext {
  readonly tree: TreeDoc;
  readonly configured: boolean;
  readonly summary: string;
  readonly recentTurns: ReadonlyArray<{ readonly role: string; readonly text: string }>;
  readonly county: string;
  readonly candidateCount: number;
  readonly qualifiedCount: number;
  readonly extraSignals: readonly string[];
}

export const buildInstructions = (context: ScoutContext): string => {
  const catalog = [
    ...Object.entries(SIGNAL_CATALOG).map(
      ([key, info]) => `- ${key}: ${info.description} (${info.format})`,
    ),
    ...context.extraSignals
      .filter((key) => SIGNAL_CATALOG[key] === undefined)
      .map((key) => `- ${key}: engine-measured signal present on current properties`),
  ].join("\n");
  const turns = context.recentTurns
    .map((turn) => `${turn.role}: ${turn.text.split("\n")[0] ?? ""}`)
    .join("\n");
  return [
    `You are Goliath Scout, the SMS assistant of Data Diver, a county public-record`,
    `ingestion engine that finds distressed properties. You manage the user's`,
    `acquisition criteria, which are a decision tree evaluated against measured`,
    `property signals. You never invent properties, signals or numbers: every fact`,
    `you state must come from the context below.`,
    ``,
    `MEASURED SIGNALS available as condition fields:`,
    catalog,
    ``,
    `CURRENT DECISION TREE (version ${context.tree.version}):`,
    JSON.stringify(context.tree.graph),
    ``,
    `SCAN STATE: county "${context.county}"; ${context.candidateCount} properties`,
    `compiled from county records, ${context.qualifiedCount} currently match the tree.`,
    ``,
    `CONVERSATION SUMMARY: ${context.summary === "" ? "(none)" : context.summary}`,
    `RECENT TURNS:`,
    turns === "" ? "(none)" : turns,
    ``,
    context.configured
      ? `The user has already configured their criteria. Apply requested changes.`
      : `FIRST-TIME USER: interview them before changing anything. Ask one short` +
        ` question at a time about: minimum amount owed, whether they require` +
        ` multi-source corroboration, minimum debt-to-value, which other measured` +
        ` signals matter to them, and whether outreach needs their approval.` +
        ` After the interview, emit the tree.`,
    ``,
    `OUTPUT CONTRACT: respond with exactly one JSON object, nothing else.`,
    `To answer or ask: {"kind":"reply","text":"<sms-length message>"}`,
    `To change criteria: {"kind":"set_tree","text":"<confirmation for the user>",`,
    `"graph":{"entry":"<node-id>","nodes":[...]}} where nodes are`,
    `{"kind":"condition","id","field":"<signal>","op":"gte|gt|lte|lt|eq","value":N,"onPass","onFail"},`,
    `{"kind":"approval","id","prompt","next"}, or {"kind":"action","id","action":"match|discard"}.`,
    `Every path must end in an action. Route failed conditions to a discard action.`,
    `Keep an approval node before match unless the user explicitly waives approval.`,
    `When the user asks to scan, add or switch to a different county, respond`,
    `{"kind":"discover","text":"<tell the user validation is starting>",`,
    `"jurisdiction":"<snake_case county id ending in the two-letter state, e.g.`,
    `chesterfield_county_va>","candidates":[{"id":"<jurisdiction>_<kind>","name":"...",`,
    `"url":"https://..."}]} with up to 4 candidate machine-readable public-record`,
    `endpoints (CSV or JSON exports: Socrata resource exports, ArcGIS query URLs,`,
    `open-data portal downloads) covering tax delinquency, assessments or code`,
    `enforcement for that county. Suggest only URLs you have genuine reason to`,
    `believe exist; every candidate is fetched and classified by the engine and`,
    `only sources that really extract records are admitted.`,
    `Keep replies under 500 characters: this is SMS.`,
  ].join("\n");
};
