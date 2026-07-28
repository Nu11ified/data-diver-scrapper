import { NextResponse } from "next/server";
import { withErrors } from "@/lib/api";
import * as engine from "@/lib/engine";
import { syncFromEngine } from "@/lib/sync";

/** POST ?rounds=N: cache-replay benchmark on the engine (live measurement). */
export async function POST(req: Request) {
  return withErrors(async () => {
    const raw = new URL(req.url).searchParams.get("rounds");
    const rounds = raw ? Number.parseInt(raw, 10) : 1;
    if (!Number.isFinite(rounds) || rounds < 1) {
      return NextResponse.json(
        { error: "bad_request", message: "rounds must be a positive integer" },
        { status: 400 },
      );
    }
    const result = await engine.benchmark(rounds);
    await syncFromEngine();
    return NextResponse.json(result);
  });
}
