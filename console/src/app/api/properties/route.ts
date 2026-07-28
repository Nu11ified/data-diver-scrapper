import { NextResponse } from "next/server";
import { withErrors } from "@/lib/api";
import { prisma } from "@/lib/db";

/** GET ?county=<slug>: properties from Postgres, optionally per county. */
export async function GET(req: Request) {
  return withErrors(async () => {
    const county = new URL(req.url).searchParams.get("county");
    const properties = await prisma.property.findMany({
      where: county ? { county: { slug: county } } : undefined,
      orderBy: { key: "asc" },
      include: { county: { select: { name: true, slug: true } } },
    });
    return NextResponse.json(properties);
  });
}
