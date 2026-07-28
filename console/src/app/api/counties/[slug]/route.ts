import { NextResponse } from "next/server";
import { withErrors } from "@/lib/api";
import { prisma } from "@/lib/db";

export async function GET(
  _req: Request,
  { params }: { params: Promise<{ slug: string }> },
) {
  return withErrors(async () => {
    const { slug } = await params;
    const county = await prisma.county.findUnique({
      where: { slug },
      include: {
        sources: {
          orderBy: { name: "asc" },
          include: {
            runs: { orderBy: { startedAt: "desc" }, take: 1 },
            repairs: { orderBy: { at: "desc" } },
          },
        },
        properties: { orderBy: { key: "asc" } },
      },
    });
    if (!county) {
      return NextResponse.json(
        { error: "not_found", message: `no county with slug "${slug}"` },
        { status: 404 },
      );
    }
    return NextResponse.json(county);
  });
}
