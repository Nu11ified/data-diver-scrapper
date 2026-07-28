import { NextResponse } from "next/server";
import { withErrors } from "@/lib/api";
import { prisma } from "@/lib/db";
import * as engine from "@/lib/engine";
import { syncFromEngine } from "@/lib/sync";

/**
 * Save operator mapping overrides (field -> source label, "" = force-unmap)
 * on the engine, which re-runs the source; mirror and return that run.
 */
export async function POST(req: Request, { params }: { params: Promise<{ id: string }> }) {
  return withErrors(async () => {
    const { id } = await params;
    const body = (await req.json()) as {
      overrides?: Record<string, string>;
    };
    if (!body.overrides || typeof body.overrides !== "object") {
      return NextResponse.json(
        { error: "bad_request", message: "overrides object is required" },
        { status: 400 },
      );
    }
    const run = await engine.setMapping(id, body.overrides);
    await syncFromEngine();
    const stored = await prisma.run.findUnique({ where: { id: run.id } });
    return NextResponse.json(stored ?? run);
  });
}
