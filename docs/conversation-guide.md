# SMS conversation guide

Data Diver should feel like one capable acquisition scout with memory, not a
form split across text messages. This guide is the product contract for prompts,
tools, state transitions, server-written replies, and conversation tests.

## Source principles

The contract follows these established guidelines:

- [Google: Learn about conversation](https://developers.google.com/assistant/conversation-design/learn-about-conversation)
  says cooperative users often provide more information than a prompt requires.
  The system must capture it instead of making them repeat it.
- [Google: Commands](https://developers.google.com/assistant/conversation-design/commands)
  recommends natural language over taught keywords and supports both guided
  dialogs and expert one-shot requests.
- [Google: Confirmations](https://developers.google.com/assistant/conversation-design/confirmations)
  recommends implicit confirmation for understood parameters, explicit
  confirmation for consequential actions, and one-step corrections without
  restarting the dialog.
- [Google: Errors](https://developers.google.com/assistant/conversation-design/errors)
  recommends assuming the user is cooperative, making repair prompts
  context-specific, avoiding verbatim repetition, and escalating help only when
  the first concise repair fails.
- [Amazon Lex: Conversation design principles](https://docs.aws.amazon.com/lexv2/latest/dg/getting-started-best-practices.html)
  recommends starting from user goals, preserving collected context, making
  corrections easy, confirming consequential actions, and providing escape
  routes.
- [Twilio: Error 21617](https://www.twilio.com/docs/api/errors/21617)
  recommends keeping SMS replies below 320 characters when possible. Twilio's
  [Messaging Services documentation](https://www.twilio.com/docs/messaging/services)
  also explains why GSM-friendly punctuation avoids unexpected segment splits.

## Conversation contract

1. Capture every relevant fact in the user's message, even when it arrives
   early, out of order, or alongside an answer to the current question.
2. Preserve accepted facts across interruptions, questions, corrections,
   failures, rejections, and later sessions.
3. Never ask for a fact that is already known. Ask only about ambiguity that
   changes the result.
4. Support both modes without making the user choose one:
   - A novice can give one answer at a time.
   - An expert can state the entire search in one message.
5. Treat corrections as edits, not rejection plus restart. "No, make it
   $20,000" changes the amount and immediately presents the revised result.
6. Confirm interpreted search parameters in a compact summary. Require explicit
   confirmation before saving criteria or scheduling outreach.
7. Accept natural confirmations and rejections. Words such as `approve` and
   `reject` may be examples, but never required commands.
8. Use safe defaults only for omitted preferences, disclose them in the summary,
   and never replace an explicit value with a default.
9. If understanding fails, preserve what was understood and ask one
   context-specific repair question. Do not repeat the same prompt verbatim.
10. If a dependency fails, say what failed, what did not change, and the one
    useful recovery action. Never claim work started when it did not.
11. Keep one clear next move per reply. Do not front-load process explanations,
    menus, or every future question.
12. Target 320 characters, use GSM-friendly punctuation, and put detailed
    property evidence behind a numbered selection rather than in the lead list.
13. Support one to five simultaneous markets. Resolve and measure each market
    independently, then present combined results with a per-market breakdown.
14. Bind a state-only answer to a market only when the conversation has an
    unresolved state question for a named place. Incidental location context
    such as "I live in Virginia" must not silently change the search.
15. A message may answer a pending question, add criteria, and ask "why" at the
    same time. Apply the answer, preserve the added criteria, explain the reason,
    and ask only for what remains.

## Scenario matrix

Every behavior below needs a regression test at the model decision boundary or
the deterministic thread boundary.

| User behavior | Required system behavior |
| --- | --- |
| `Hi` | One short value statement and one invitation to give the whole search or only the market. |
| `Norfolk` | Ask for the state because the place is ambiguous; retain any other supplied criteria. |
| `I want to see two counties at the same time` | Confirm multi-market comparison and ask for both places in one question. |
| `Norfolk and Cincinnati` | Ask which state belongs to each place; do not guess or serialize the questions. |
| `Norfolk is Virginia and Cincinnati is Ohio. Why do you need states?` | Store both normalized markets, explain that states select the correct official record systems, and continue from the remaining criteria. |
| `I live in Virginia, but invest nationwide` | Treat Virginia as personal context unless an unresolved market-state question makes it an answer. Do not change the search silently. |
| One market is ready and another is still loading | Preserve the portfolio, show per-market progress, and do not report a combined count until every market has the required signals. |
| Combined lead list | Report the measured count for each market and include a strong result from every market before filling the remaining ranked slots. |
| Full search in one message | Capture all constraints, default only omissions, and present one confirmation. |
| Criteria arrive out of order | Store every usable value and ask once for what is truly missing. |
| `Whatever you recommend` | Apply disclosed safe defaults and move forward. |
| `No assessed minimum` | Store zero; do not replace it with a positive default. |
| `No, make it $20k` | Revise the pending search in one step without clearing other criteria. |
| Bare rejection | Apply nothing, retain onboarding details for later edits, and do not force a restart. |
| User asks a question mid-setup | Answer it without losing setup state, then return to the relevant next move. |
| User returns later | Resume from persisted facts and pending state without replaying the introduction. |
| Natural approval or rejection | Resolve the pending action without requiring exact keywords. |
| Provider or source failure | State the failed dependency, preserve state, avoid false results, and offer one recovery action. |
| No qualified properties | Report zero only when required source coverage is complete. |
| Lead list | Send a compact ranked list; reveal owner, evidence, valuation, and outreach after selection. |
| User corrects an outreach draft | Keep the property and evidence context, revise the draft, and request approval again. |
| Account deletion or outreach | Require explicit confirmation appropriate to the consequence. |

## Review checklist

Before shipping a conversation change:

- Read the reply aloud as a two-person exchange.
- Test the happy path, expert one-shot path, correction path, interruption path,
  ambiguous-input path, dependency-failure path, and resume path.
- Verify the state after every turn, not only the reply text.
- Verify rendered server replies as well as model-authored replies.
- Check the final outbound SMS length and punctuation after interpolation.
- Confirm that no response teaches internal tool names, schema fields, or
  mandatory keywords.
