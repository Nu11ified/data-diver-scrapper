import { NextResponse } from "next/server";
import { withErrors } from "@/lib/api";
import { prisma } from "@/lib/db";
import * as engine from "@/lib/engine";
import { syncFromEngine } from "@/lib/sync";

/** Run every enabled source on the engine, mirror, return latest runs. */
export async function POST() {
  return withErrors(async () => {
    await engine.runAll();
    const stats = await syncFromEngine();
    const runs = await prisma.run.findMany({
      orderBy: { startedAt: "desc" },
      take: 50,
    });
    return NextResponse.json({ synced: stats, runs });
  });
}
