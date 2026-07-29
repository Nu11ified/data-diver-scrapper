import { describe, expect, test } from "bun:test";

import { isDeterministicCommand } from "./conversation/thread.ts";

describe("destructive commands never reach the model", () => {
  test("reset and delete are recognised in every form the user might type", () => {
    for (const form of [
      "reset",
      "RESET",
      " reset ",
      "/reset-account",
      "reset account",
      "/reset_account",
      "delete",
      "delete account",
      "/delete-account",
      "DELETE ACCOUNT",
    ]) {
      expect(isDeterministicCommand(form)).toBe(true);
    }
  });

  test("ordinary conversation still routes to the model", () => {
    for (const form of [
      "can you reset my minimum owed to 40000",
      "delete the third property from the list",
      "scan chesterfield county",
    ]) {
      expect(isDeterministicCommand(form)).toBe(false);
    }
  });
});
