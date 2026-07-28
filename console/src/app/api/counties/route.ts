import { NextResponse } from "next/server";
import { withErrors } from "@/lib/api";
import { prisma } from "@/lib/db";

export async function GET() {
  return withErrors(async () => {
    const counties = await prisma.county.findMany({
      orderBy: { name: "asc" },
      include: {
        _count: { select: { sources: true, properties: true } },
        sources: {
          select: { id: true, name: true, enabled: true, lastRunAt: true },
        },
      },
    });
    return NextResponse.json(counties);
  });
}
