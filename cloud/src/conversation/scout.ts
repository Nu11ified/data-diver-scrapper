import { SIGNAL_CATALOG, type Graph, type TreeDoc } from "../decision/graph.ts";
import type { AcquisitionProfile, ProfileUpdate } from "./profile.ts";

export interface SourceCandidate {
  readonly id: string;
  readonly name: string;
  readonly url: string;
}

export type ScoutDecision =
  | { readonly kind: "reply"; readonly text: string }
  | {
      readonly kind: "update_profile";
      readonly text: string;
      readonly update: ProfileUpdate;
    }
  | {
      readonly kind: "resolve_pending";
      readonly text: string;
      readonly approved: boolean;
    }
  | { readonly kind: "set_tree"; readonly text: string; readonly graph: Graph }
  | {
      readonly kind: "temp_filter";
      readonly text: string;
      readonly graph: Graph;
      readonly limit?: number;
    }
  | {
      readonly kind: "remember_filter";
      readonly text: string;
      readonly remember: boolean;
    }
  | {
      readonly kind: "show_matches";
      readonly text: string;
      readonly limit?: number;
    }
  | {
      readonly kind: "show_property";
      readonly text: string;
      readonly index: number;
    }
  | {
      readonly kind: "discover";
      readonly text: string;
      readonly jurisdiction: string;
      readonly candidates: readonly SourceCandidate[];
    };

export interface ScoutContext {
  readonly tree: TreeDoc;
  readonly configured: boolean;
  readonly summary: string;
  readonly recentTurns: ReadonlyArray<{
    readonly role: string;
    readonly text: string;
    readonly at: string;
  }>;
  readonly county: string;
  readonly candidateCount: number;
  readonly qualifiedCount: number;
  readonly extraSignals: readonly string[];
  readonly profile?: AcquisitionProfile;
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
  return [
    `You are Data Diver, an SMS assistant over a county public-record`,
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
    `BUYER PROFILE: ${JSON.stringify(context.profile ?? {})}`,
    ``,
    context.configured
      ? `The user has configured criteria. Use the tools to inspect leads, explain facts, or propose requested changes.`
      : `The user is onboarding. Use update_profile for every answer, converting natural language into normalized fields. The server knows which question comes next. Never reject an answer merely because it does not match a keyword. If the user delegates a choice, make and explain a conservative professional default. Do not propose a tree yourself during onboarding; the server builds it from the completed profile.`,
    ``,
    `You are the whole conversation. Greetings, questions, ambiguity and corrections all go through you. Choose exactly one tool for each message. Never expose command menus, JSON, tool names, internal state, or implementation details.`,
    `Use resolve_pending when the user accepts or rejects a pending proposal, draft, or temporary filter.`,
    `Use show_matches and show_property only for facts the engine will supply.`,
    `Use propose_tree only for post-onboarding saved criteria changes.`,
    `Every path must end in an action. Route failed conditions to a discard action.`,
    `Keep an approval node before match unless the user explicitly waives approval.`,
    `A proposed tree never applies immediately. The system evaluates it`,
    `against the user's current properties, shows what would be added or removed`,
    `compared to the active tree, and asks the user to reply APPROVE or REJECT`,
    `before anything changes. Write "text" as a lead-in sentence describing the`,
    `change, not as a confirmation that it is already in effect.`,
    `Use propose_tree only when the user clearly wants the change kept.`,
    `When the request is provisional, do not touch saved criteria. Use`,
    `temporary_filter with the same full graph: it is`,
    `applied to this turn only, the matches are listed, and the user is asked`,
    `whether to keep it. Build that graph by editing the CURRENT DECISION TREE`,
    `above, not from scratch, so everything they already asked for survives.`,
    `Use remember_filter to keep or discard it after the user answers.`,
    `When the user asks to scan, add or switch county after onboarding, use`,
    `discover_sources with a snake_case jurisdiction ending in the state code and`,
    `up to 4 candidate machine-readable public-record`,
    `endpoints (CSV or JSON exports: Socrata resource exports, ArcGIS query URLs,`,
    `open-data portal downloads) covering tax delinquency, assessments or code`,
    `enforcement for that county. Suggest only URLs you have genuine reason to`,
    `believe exist; every candidate is fetched and classified by the engine and`,
    `only sources that really extract records are admitted.`,
    `Keep replies under 500 characters: this is SMS.`,
  ].join("\n");
};
