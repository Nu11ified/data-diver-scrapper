import { NextResponse } from "next/server";
import { withErrors } from "@/lib/api";
import { prisma } from "@/lib/db";
import * as engine from "@/lib/engine";
import { syncFromEngine } from "@/lib/sync";

/** POST {county,name,url}: create the source on the engine, then mirror. */
export async function POST(req: Request) {
  return withErrors(async () => {
    const body = (await req.json()) as {
      county?: string;
      name?: string;
      url?: string;
    };
    if (!body.county || !body.name || !body.url) {
      return NextResponse.json(
        { error: "bad_request", message: "county, name and url are required" },
        { status: 400 },
      );
    }
    const created = await engine.createSource({
      name: body.name,
      url: body.url,
      jurisdiction: body.county,
    });
    await syncFromEngine();
    const source = await prisma.source.findUnique({
      where: { id: created.id },
      include: { county: true },
    });
    return NextResponse.json(source ?? created, { status: 201 });
  });
}
