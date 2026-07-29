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
    kind: Schema.Literal("temp_filter"),
    text: Schema.String,
    graph: Graph,
    limit: Schema.optional(Schema.Number),
  }),
  Schema.Struct({
    kind: Schema.Literal("remember_filter"),
    text: Schema.String,
    remember: Schema.Boolean,
  }),
  Schema.Struct({
    kind: Schema.Literal("show_matches"),
    text: Schema.String,
    limit: Schema.optional(Schema.Number),
  }),
  Schema.Struct({
    kind: Schema.Literal("show_property"),
    text: Schema.String,
    index: Schema.Number,
  }),
  Schema.Struct({
    kind: Schema.Literal("approve_outreach"),
    text: Schema.String,
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
      : `FIRST-TIME USER. Interview them, one short question per message, and do` +
        ` not emit a tree until you have enough to build a real one. Cover, in` +
        ` this order:` +
        ` (1) which county or city they buy in - ask for the state too, and when` +
        ` they name one that is not the county in SCAN STATE, answer with a` +
        ` discover decision so the engine goes and finds its records;` +
        ` (2) the minimum amount owed that makes a lead worth their time;` +
        ` (3) roughly what a property has to be worth for them to care, which` +
        ` maps to the assessed signal;` +
        ` (4) whether a lead must be corroborated by more than one county source;` +
        ` (5) which other measured signals above matter to them, naming the ones` +
        ` that actually exist rather than inventing any;` +
        ` (6) whether outreach needs their approval before it is sent.` +
        ` Acknowledge each answer in one line before asking the next. When you` +
        ` have them, emit set_tree built only from signals in the catalogue` +
        ` above, and say in the text which of their answers became which` +
        ` condition. This tree is theirs alone: never assume another user's` +
        ` numbers.`,
    ``,
    `OUTPUT CONTRACT: respond with exactly one JSON object, nothing else.`,
    `You are the whole conversation: greetings, questions, small talk and`,
    `every request go through you. There is no keyword menu behind you, so`,
    `never tell the user to type a command word.`,
    ``,
    `To answer or ask: {"kind":"reply","text":"<sms-length message>"}`,
    `To list the properties that match: {"kind":"show_matches","text":"<lead-in>","limit":3}`,
    `To open one of the properties you last listed, one-based:`,
    `{"kind":"show_property","text":"<lead-in>","index":1}`,
    `To send the outreach the user just approved:`,
    `{"kind":"approve_outreach","text":"<confirmation>"}`,
    `To change criteria: {"kind":"set_tree","text":"<confirmation for the user>",`,
    `"graph":{"entry":"<node-id>","nodes":[...]}} where nodes are`,
    `{"kind":"condition","id","field":"<signal>","op":"gte|gt|lte|lt|eq","value":N,"onPass","onFail"},`,
    `{"kind":"approval","id","prompt","next"}, or {"kind":"action","id","action":"match|discard"}.`,
    `Every path must end in an action. Route failed conditions to a discard action.`,
    `Keep an approval node before match unless the user explicitly waives approval.`,
    `Use set_tree only when the user clearly wants the change kept.`,
    `When the request sounds provisional - "for now", "just this once", "also`,
    `show me", "what if", "today only", "drop X for a second" - do not touch`,
    `their saved criteria. Send the same full graph as`,
    `{"kind":"temp_filter","text":"<lead-in>","graph":{...},"limit":3}: it is`,
    `applied to this turn only, the matches are listed, and the user is asked`,
    `whether to keep it. Build that graph by editing the CURRENT DECISION TREE`,
    `above, not from scratch, so everything they already asked for survives.`,
    `When they answer that question, send`,
    `{"kind":"remember_filter","text":"<one line>","remember":true} to promote`,
    `the filter you just showed into their saved criteria, or "remember":false`,
    `to drop it and leave the saved criteria alone.`,
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
