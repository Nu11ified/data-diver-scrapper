import { NextResponse } from "next/server";
import { withErrors } from "@/lib/api";
import { prisma } from "@/lib/db";

export async function GET(
  _req: Request,
  { params }: { params: Promise<{ key: string }> },
) {
  return withErrors(async () => {
    const { key } = await params;
    const property = await prisma.property.findUnique({
      where: { key },
      include: {
        county: { select: { name: true, slug: true } },
        events: {
          orderBy: { recordedAt: "desc" },
          include: { source: { select: { id: true, name: true } } },
        },
      },
    });
    if (!property) {
      return NextResponse.json(
        { error: "not_found", message: `no property with key "${key}"` },
        { status: 404 },
      );
    }
    return NextResponse.json(property);
  });
}
