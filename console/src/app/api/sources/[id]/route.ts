import { NextResponse } from "next/server";
import { withErrors } from "@/lib/api";
import { prisma } from "@/lib/db";
import * as engine from "@/lib/engine";
import { syncFromEngine } from "@/lib/sync";

type Ctx = { params: Promise<{ id: string }> };

/** Source + latest snapshot fields + runs + repairs, from Postgres. */
export async function GET(_req: Request, { params }: Ctx) {
  return withErrors(async () => {
    const { id } = await params;
    const source = await prisma.source.findUnique({
      where: { id },
      include: {
        county: true,
        runs: { orderBy: { startedAt: "desc" } },
        repairs: { orderBy: { at: "desc" } },
        records: { orderBy: { position: "asc" } },
      },
    });
    if (!source) {
      return NextResponse.json(
        { error: "not_found", message: `no source with id "${id}"` },
        { status: 404 },
      );
    }
    return NextResponse.json(source);
  });
}

/** Partial update on the engine (name/url/jurisdiction/enabled), then mirror. */
export async function PATCH(req: Request, { params }: Ctx) {
  return withErrors(async () => {
    const { id } = await params;
    const body = (await req.json()) as {
      name?: string;
      url?: string;
      jurisdiction?: string;
      enabled?: boolean;
    };
    await engine.updateSource({ id, ...body });
    await syncFromEngine();
    const source = await prisma.source.findUnique({
      where: { id },
      include: { county: true },
    });
    return NextResponse.json(source);
  });
}

/** Delete on the engine, then delete the mirrored row (cascades locally). */
export async function DELETE(_req: Request, { params }: Ctx) {
  return withErrors(async () => {
    const { id } = await params;
    await engine.deleteSource(id);
    await prisma.source.deleteMany({ where: { id } });
    return NextResponse.json({ ok: true });
  });
}
