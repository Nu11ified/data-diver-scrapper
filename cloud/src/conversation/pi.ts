import {
  Agent,
  type AgentMessage,
  type AgentTool,
} from "@earendil-works/pi-agent-core";
import {
  Type,
  type Model,
  type TSchema,
} from "@earendil-works/pi-ai";
import { streamSimple as streamOpenAICodex } from "@earendil-works/pi-ai/api/openai-codex-responses";
import { openaiCodexProvider } from "@earendil-works/pi-ai/providers/openai-codex";

import { CHAT_MODEL } from "../codex/client.ts";
import type { HttpFetch } from "../codex/egress-fetch.ts";
import type { Graph } from "../decision/graph.ts";
import {
  nextProfileField,
  type ProfileField,
  type ProfileUpdate,
} from "./profile.ts";
import type { ScoutDecision, ScoutContext } from "./scout.ts";
import { buildInstructions } from "./scout.ts";

const ConditionNode = Type.Object({
  kind: Type.Literal("condition"),
  id: Type.String(),
  field: Type.String(),
  op: Type.Union([
    Type.Literal("gte"),
    Type.Literal("gt"),
    Type.Literal("lte"),
    Type.Literal("lt"),
    Type.Literal("eq"),
  ]),
  value: Type.Number(),
  onPass: Type.String(),
  onFail: Type.String(),
});

const ApprovalNode = Type.Object({
  kind: Type.Literal("approval"),
  id: Type.String(),
  prompt: Type.String(),
  next: Type.String(),
});

const ActionNode = Type.Object({
  kind: Type.Literal("action"),
  id: Type.String(),
  action: Type.Union([Type.Literal("match"), Type.Literal("discard")]),
});

const GraphInput = Type.Object({
  entry: Type.String(),
  nodes: Type.Array(Type.Union([ConditionNode, ApprovalNode, ActionNode])),
});

const MessageText = Type.String({ minLength: 1, maxLength: 320 });
const Text = Type.Object({ text: MessageText });
const County = Type.String({
  pattern: "^[^,\\n]+,\\s*[A-Z]{2}$",
  description:
    'The user market normalized as "City or County, ST" with an uppercase two-letter state.',
});

export interface PiScoutCall {
  readonly accessToken: string;
  readonly sessionId: string;
  readonly userText: string;
  readonly context: ScoutContext;
  readonly fetch?: HttpFetch;
}

export interface PiScoutResult {
  readonly decision: ScoutDecision;
  readonly messages: readonly unknown[];
}

export interface PiScoutRuntime {
  readonly model: Model<any>;
  readonly streamFn: ConstructorParameters<typeof Agent>[0]["streamFn"];
}

const productionRuntime = (
  requestFetch: HttpFetch = globalThis.fetch,
): PiScoutRuntime => {
  const provider = openaiCodexProvider();
  const model = provider.getModels().find((candidate) => candidate.id === CHAT_MODEL);
  if (model === undefined) {
    throw new Error(`Pi model catalog does not contain openai-codex/${CHAT_MODEL}`);
  }
  return {
    model,
    streamFn: (selected, context, options) =>
      streamOpenAICodex(selected as typeof model, context, {
        ...options,
        fetch: requestFetch as typeof globalThis.fetch,
      }),
  };
};

const emptyUsage = {
  input: 0,
  output: 0,
  cacheRead: 0,
  cacheWrite: 0,
  totalTokens: 0,
  cost: {
    input: 0,
    output: 0,
    cacheRead: 0,
    cacheWrite: 0,
    total: 0,
  },
};

const historyMessages = (context: ScoutContext): AgentMessage[] =>
  context.recentTurns.map((turn) =>
    turn.role === "user"
      ? {
          role: "user" as const,
          content: turn.text,
          timestamp: Date.parse(turn.at),
        }
      : {
          role: "assistant" as const,
          content: [{ type: "text" as const, text: turn.text }],
          api: "openai-codex-responses" as const,
          provider: "openai-codex",
          model: CHAT_MODEL,
          usage: emptyUsage,
          stopReason: "stop" as const,
          timestamp: Date.parse(turn.at),
        },
  );

const textOf = (message: unknown): string => {
  if (
    typeof message !== "object" ||
    message === null ||
    !("role" in message) ||
    message.role !== "assistant" ||
    !("content" in message) ||
    !Array.isArray(message.content)
  ) {
    return "";
  }
  return message.content
    .flatMap((part) =>
      typeof part === "object" &&
      part !== null &&
      "type" in part &&
      part.type === "text" &&
      "text" in part &&
      typeof part.text === "string"
        ? [part.text]
        : [],
    )
    .join("")
    .trim();
};

const toolResult = <T>(details: T) => ({
  content: [{ type: "text" as const, text: "Accepted." }],
  details,
  terminate: true,
});

const defineTool = <TParameters extends TSchema>(
  tool: AgentTool<TParameters>,
): AgentTool<TParameters> => tool;

const fieldsIn = (update: ProfileUpdate): readonly ProfileField[] => [
  ...(update.county === undefined && update.markets === undefined
    ? []
    : ["county" as const]),
  ...(update.minOwed === undefined ? [] : ["minOwed" as const]),
  ...(update.minAssessed === undefined ? [] : ["minAssessed" as const]),
  ...(update.evidence === undefined ? [] : ["evidence" as const]),
  ...(update.maxDaysSinceEvent === undefined && update.anyEventAge === undefined
    ? []
    : ["recency" as const]),
  ...(update.requireApproval === undefined
    ? []
    : ["requireApproval" as const]),
];

const explicitProfileUpdate = (text: string): ProfileUpdate => {
  const amountMatch =
    /\b(?:minimum(?:\s+amount)?\s+owed|at\s+least)\b[^$\d]{0,24}\$?\s*([\d,.]+)\s*([kK])?/i.exec(
      text,
    );
  const minOwed =
    amountMatch === null
      ? undefined
      : Number(amountMatch[1]?.replace(/,/g, "")) *
        (amountMatch[2] === undefined ? 1 : 1_000);
  const requireApproval =
    /\b(?:without|no)\s+(?:my\s+)?approval\b/i.test(text)
      ? false
      : /\b(?:require|requiring|required|use)\s+(?:my\s+)?approval\b|\bapproval\s+before\s+outreach\b/i.test(
            text,
          )
        ? true
        : undefined;
  return {
    ...(Number.isFinite(minOwed) ? { minOwed } : {}),
    ...(/\b(?:no|without)\s+(?:assessed(?:-value)?|assessment(?:-value)?)\s+(?:minimum|floor)\b/i.test(
      text,
    )
      ? { minAssessed: 0 }
      : {}),
    ...(/\b(?:allow|use|keep|include)\s+(?:records?\s+of\s+)?any\s+event\s+age\b/i.test(
      text,
    )
      ? { anyEventAge: true }
      : {}),
    ...(/\b(?:evidence\s+from|require|use)\s+(?:multiple|two|2)\s+(?:independent\s+)?(?:county\s+)?sources\b/i.test(
      text,
    )
      ? { evidence: "multiple_sources" as const }
      : {}),
    ...(requireApproval === undefined ? {} : { requireApproval }),
  };
};

const validateProfileUpdate = (
  current: PiScoutCall["context"]["profile"],
  update: ProfileUpdate,
  completeWithDefaults: boolean,
): void => {
  const expected = nextProfileField(current);
  const fields = fieldsIn(update);
  if (
    fields.length === 0 &&
    (!completeWithDefaults || expected === undefined)
  ) {
    throw new Error(
      "Capture every search preference the user supplied, or reply naturally if they supplied none.",
    );
  }
  if (
    update.county !== undefined &&
    !/^[^,\n]+,\s*[A-Z]{2}$/.test(update.county.trim())
  ) {
    throw new Error(
      'Keep the place and state from the user, normalize it as "City or County, ST", and call update_profile again.',
    );
  }
  if (
    update.markets !== undefined &&
    (update.markets.length === 0 ||
      update.markets.length > 5 ||
      update.markets.some(
        (market) => !/^[^,\n]+,\s*[A-Z]{2}$/.test(market.trim()),
      ))
  ) {
    throw new Error(
      'Keep every place and state from the user, normalize each as "City or County, ST", and call update_profile again.',
    );
  }
  if (
    update.maxDaysSinceEvent !== undefined &&
    update.anyEventAge !== undefined
  ) {
    throw new Error(
      "Choose either a numeric recency window or any event age, not both.",
    );
  }
  if (update.anyEventAge === false) {
    throw new Error("Use anyEventAge=true, or provide a positive maxDaysSinceEvent.");
  }
};

const authorizesDefaults = (text: string): boolean =>
  /\b(?:safe\s+defaults?|use\s+(?:the\s+)?defaults?|default\s+the\s+rest|whatever\s+works|you\s+decide|handle\s+the\s+rest)\b/i.test(
    text,
  ) ||
  /\b(?:find|search|scan|show|compare|target|look(?:ing)?\s+for)\b/i.test(text);

export const runPiScout = async (
  call: PiScoutCall,
  runtime: PiScoutRuntime = productionRuntime(call.fetch),
): Promise<PiScoutResult> => {
  let decision: ScoutDecision | undefined;
  const choose = <T extends ScoutDecision>(next: T) => {
    if (decision === undefined) decision = next;
    return toolResult(next);
  };
  const tools: AgentTool<any>[] = [
    defineTool({
      name: "reply",
      label: "Reply",
      description:
        "Send a concise, structured SMS that answers directly, shows relevant progress, and ends with one contextual next move without changing state.",
      parameters: Text,
      execute: async (_id, params) => {
        const update = explicitProfileUpdate(call.userText);
        return fieldsIn(update).length === 0
          ? choose({ kind: "reply", text: params.text as string })
          : choose({
              kind: "update_profile",
              text: params.text as string,
              update,
            });
      },
    }),
    defineTool({
      name: "update_profile",
      label: "Update acquisition profile",
      description:
        `Record every onboarding preference in the latest message, regardless of the current field (${nextProfileField(call.context.profile) ?? "complete"}). Never drop values that arrive early. Use markets for one to five simultaneous markets and preserve their order. A message can answer a prior location question and ask "why"; capture the answer and address the question in text. Set completeWithDefaults=true when the user gives usable markets and asks for a search without requesting an interview, or delegates unspecified choices. Defaults fill only unanswered preferences: $10,000 owed, no assessed-value floor, multiple independent sources, any event age, and approval before outreach. Explicit values always win. If anything remains missing, acknowledge all captured values and ask one question for everything else, offering defaults. If complete, acknowledge once without a question; the server shows the full search and confirmation.`,
      parameters: Type.Object({
        text: MessageText,
        county: Type.Optional(County),
        markets: Type.Optional(
          Type.Array(County, { minItems: 1, maxItems: 5 }),
        ),
        minOwed: Type.Optional(Type.Number({ minimum: 0 })),
        minAssessed: Type.Optional(Type.Number({ minimum: 0 })),
        evidence: Type.Optional(
          Type.Union([
            Type.Literal("multiple_sources"),
            Type.Literal("open_violation"),
            Type.Literal("both"),
            Type.Literal("neither"),
          ]),
        ),
        maxDaysSinceEvent: Type.Optional(Type.Number({ minimum: 1 })),
        anyEventAge: Type.Optional(Type.Literal(true)),
        requireApproval: Type.Optional(Type.Boolean()),
        completeWithDefaults: Type.Optional(Type.Boolean()),
      }),
      execute: async (_id, params) => {
        const update: ProfileUpdate = {
          ...("county" in params ? { county: params.county as string } : {}),
          ...("markets" in params
            ? { markets: params.markets as readonly string[] }
            : {}),
          ...("minOwed" in params ? { minOwed: params.minOwed as number } : {}),
          ...("minAssessed" in params
            ? { minAssessed: params.minAssessed as number }
            : {}),
          ...("evidence" in params
            ? { evidence: params.evidence as ProfileUpdate["evidence"] }
            : {}),
          ...("maxDaysSinceEvent" in params
            ? { maxDaysSinceEvent: params.maxDaysSinceEvent as number }
            : {}),
          ...("anyEventAge" in params
            ? { anyEventAge: params.anyEventAge as boolean }
            : {}),
          ...("requireApproval" in params
            ? { requireApproval: params.requireApproval as boolean }
            : {}),
          ...explicitProfileUpdate(call.userText),
        };
        const completeWithDefaults =
          params.completeWithDefaults === true && authorizesDefaults(call.userText);
        if (fieldsIn(update).length === 0 && !completeWithDefaults) {
          return choose({
            kind: "reply",
            text: params.text as string,
          });
        }
        validateProfileUpdate(
          call.context.profile,
          update,
          completeWithDefaults,
        );
        return choose({
          kind: "update_profile",
          text: params.text as string,
          update,
          ...(completeWithDefaults ? { completeWithDefaults: true } : {}),
        });
      },
    }),
    defineTool({
      name: "show_matches",
      label: "Show matching properties",
      description:
        "List the strongest current properties when required signals are available. Use this for a verified partial set too; the server labels its count as a floor.",
      parameters: Type.Object({
        text: MessageText,
        limit: Type.Optional(Type.Number({ minimum: 1, maximum: 10 })),
      }),
      execute: async (_id, params) =>
        choose({
          kind: "show_matches",
          text: params.text as string,
          ...("limit" in params ? { limit: params.limit as number } : {}),
        }),
    }),
    defineTool({
      name: "show_property",
      label: "Show property",
      description: "Open a one-based property number from the most recent lead list.",
      parameters: Type.Object({ text: MessageText, index: Type.Number({ minimum: 1 }) }),
      execute: async (_id, params) =>
        choose({
          kind: "show_property",
          text: params.text as string,
          index: params.index as number,
        }),
    }),
    defineTool({
      name: "resolve_pending",
      label: "Resolve pending approval",
      description:
        "Approve or reject the tree, outreach draft, or temporary filter currently awaiting the user's decision.",
      parameters: Type.Object({ text: MessageText, approved: Type.Boolean() }),
      execute: async (_id, params) =>
        choose({
          kind: "resolve_pending",
          text: params.text as string,
          approved: params.approved as boolean,
        }),
    }),
    defineTool({
      name: "revise_outreach",
      label: "Revise outreach draft",
      description:
        "Revise the pending outreach draft without losing the selected property. Use only when PENDING ACTION is approve_outreach.",
      parameters: Type.Object({
        text: MessageText,
        instruction: Type.String({ minLength: 1, maxLength: 240 }),
      }),
      execute: async (_id, params) =>
        choose({
          kind: "revise_outreach",
          text: params.text as string,
          instruction: params.instruction as string,
        }),
    }),
    defineTool({
      name: "propose_tree",
      label: "Propose decision tree",
      description:
        "Propose a persistent criteria change. The server validates and previews it; it never applies without user confirmation.",
      parameters: Type.Object({ text: MessageText, graph: GraphInput }),
      execute: async (_id, params) =>
        choose({
          kind: "set_tree",
          text: params.text as string,
          graph: params.graph as Graph,
        }),
    }),
    defineTool({
      name: "temporary_filter",
      label: "Preview temporary filter",
      description:
        "Apply a provisional criteria change to this response only without changing saved criteria.",
      parameters: Type.Object({
        text: MessageText,
        graph: GraphInput,
        limit: Type.Optional(Type.Number({ minimum: 1, maximum: 10 })),
      }),
      execute: async (_id, params) =>
        choose({
          kind: "temp_filter",
          text: params.text as string,
          graph: params.graph as Graph,
          ...("limit" in params ? { limit: params.limit as number } : {}),
        }),
    }),
    defineTool({
      name: "remember_filter",
      label: "Remember temporary filter",
      description: "Keep or discard the last temporary filter after the user answers.",
      parameters: Type.Object({ text: MessageText, remember: Type.Boolean() }),
      execute: async (_id, params) =>
        choose({
          kind: "remember_filter",
          text: params.text as string,
          remember: params.remember as boolean,
        }),
    }),
    defineTool({
      name: "discover_sources",
      label: "Discover county sources",
      description:
        "Validate public-record endpoints when the user asks to add or switch markets.",
      parameters: Type.Object({
        text: MessageText,
        jurisdiction: Type.String(),
        candidates: Type.Array(
          Type.Object({
            id: Type.String(),
            name: Type.String(),
            url: Type.String(),
          }),
          { maxItems: 4 },
        ),
      }),
      execute: async (_id, params) =>
        choose({
          kind: "discover",
          text: params.text as string,
          jurisdiction: params.jurisdiction as string,
          candidates: params.candidates,
        }),
    }),
  ];
  const availableTools = call.context.configured
    ? tools
    : tools.filter(
        (tool) =>
          tool.name === "reply" ||
          tool.name === "update_profile" ||
          tool.name === "resolve_pending",
      );

  const agent = new Agent({
    initialState: {
      systemPrompt: buildInstructions(call.context),
      model: runtime.model,
      thinkingLevel: "medium",
      tools: availableTools,
      messages: historyMessages(call.context),
    },
    streamFn: runtime.streamFn,
    getApiKey: () => call.accessToken,
    sessionId: call.sessionId,
    transport: "sse",
    toolExecution: "sequential",
    transformContext: async (messages) => messages.slice(-20),
  });
  const historyLength = agent.state.messages.length;
  await agent.prompt(call.userText);
  const messages = agent.state.messages;
  const newMessages = messages.slice(historyLength);
  const assistantError =
    [...newMessages]
      .reverse()
      .find(
        (message) =>
          typeof message === "object" &&
          message !== null &&
          "role" in message &&
          message.role === "assistant" &&
          "errorMessage" in message &&
          typeof message.errorMessage === "string",
      );
  const errorMessage =
    agent.state.errorMessage ??
    (assistantError !== undefined &&
    "errorMessage" in assistantError &&
    typeof assistantError.errorMessage === "string"
      ? assistantError.errorMessage
      : undefined);
  if (errorMessage !== undefined && errorMessage !== "") {
    throw new Error(`Pi agent failed: ${errorMessage}`);
  }
  const reply =
    [...newMessages]
      .reverse()
      .map(textOf)
      .find((text) => text !== "") ?? "";
  if (decision === undefined && reply === "") {
    const events = newMessages.map((message) => {
      if (
        typeof message !== "object" ||
        message === null ||
        !("role" in message) ||
        typeof message.role !== "string"
      ) {
        return "unknown";
      }
      if (
        message.role === "assistant" &&
        "content" in message &&
        Array.isArray(message.content)
      ) {
        const content = message.content.map((part) =>
          typeof part === "object" &&
          part !== null &&
          "type" in part &&
          typeof part.type === "string"
            ? part.type === "toolCall" &&
              "name" in part &&
              typeof part.name === "string"
              ? `toolCall:${part.name}`
              : part.type
            : "unknown",
        );
        const stopReason =
          "stopReason" in message && typeof message.stopReason === "string"
            ? message.stopReason
            : "unknown";
        return `assistant(${stopReason})[${content.join(",")}]`;
      }
      if (message.role === "toolResult") {
        const toolName =
          "toolName" in message && typeof message.toolName === "string"
            ? message.toolName
            : "unknown";
        const isError =
          "isError" in message && typeof message.isError === "boolean"
            ? message.isError
            : false;
        return `toolResult:${toolName}${isError ? ":error" : ""}`;
      }
      return message.role;
    });
    throw new Error(`Pi agent ended without an action: ${events.join(" -> ")}`);
  }
  const finalDecision =
    decision ??
    ({
      kind: "reply",
      text: reply,
    } satisfies ScoutDecision);
  return { decision: finalDecision, messages };
};
