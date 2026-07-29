import { describe, expect, test } from "bun:test";

import { isDeterministicCommand, slashCommand } from "./conversation/thread.ts";

describe("only slash commands bypass the model", () => {
  test("the commands that must never be interpreted", () => {
    expect(slashCommand("/connect")).toBe("connect");
    expect(slashCommand("/login")).toBe("login");
    expect(slashCommand("/logout")).toBe("logout");
    expect(slashCommand("/reset")).toBe("reset");
    expect(slashCommand("/reset-account")).toBe("reset-account");
    expect(slashCommand("/reset_account")).toBe("reset-account");
    expect(slashCommand("/delete account")).toBe("delete");
    expect(slashCommand("  /HELP  ")).toBe("help");
  });

  test("everything a person would actually say goes to the model", () => {
    // The bug this replaced answered "Hi" with a keyword menu.
    for (const said of [
      "Hi",
      "hello there",
      "review",
      "criteria",
      "approve",
      "what have you got for me",
      "scan orlando",
      "1",
      "connect",
      "reset",
    ]) {
      expect(slashCommand(said)).toBe("");
      expect(isDeterministicCommand(said)).toBe(false);
    }
  });

  test("an unknown slash word is conversation, not a command", () => {
    expect(slashCommand("/wat")).toBe("");
    expect(slashCommand("/")).toBe("");
  });
});
