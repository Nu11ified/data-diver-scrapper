import { NextResponse } from "next/server";
import { withErrors } from "@/lib/api";
import { prisma } from "@/lib/db";
import * as engine from "@/lib/engine";
import { syncFromEngine } from "@/lib/sync";

/** POST {approve}: resolve a pending repair on the engine, then mirror. */
export async function POST(req: Request, { params }: { params: Promise<{ id: string }> }) {
  return withErrors(async () => {
    const { id } = await params;
    const body = (await req.json()) as { approve?: boolean };
    if (typeof body.approve !== "boolean") {
      return NextResponse.json(
        { error: "bad_request", message: "approve (boolean) is required" },
        { status: 400 },
      );
    }
    await engine.resolveRepair(id, body.approve);
    await syncFromEngine();
    const repair = await prisma.repair.findUnique({ where: { id } });
    return NextResponse.json(repair ?? { ok: true });
  });
}
