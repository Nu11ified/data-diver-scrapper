import { NextResponse } from "next/server";
import { withErrors } from "@/lib/api";
import { prisma } from "@/lib/db";

export async function GET() {
  return withErrors(async () => {
    const reports = await prisma.trainingReport.findMany({
      orderBy: { at: "desc" },
    });
    return NextResponse.json(reports);
  });
}
