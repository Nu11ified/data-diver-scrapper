import { NextResponse } from "next/server";
import { withErrors } from "@/lib/api";
import { prisma } from "@/lib/db";
import * as engine from "@/lib/engine";
import { syncFromEngine } from "@/lib/sync";

/** Run the source on the engine, mirror results, return the run. */
export async function POST(_req: Request, { params }: { params: Promise<{ id: string }> }) {
  return withErrors(async () => {
    const { id } = await params;
    const run = await engine.runSource(id);
    await syncFromEngine();
    const stored = await prisma.run.findUnique({ where: { id: run.id } });
    return NextResponse.json(stored ?? run);
  });
}
