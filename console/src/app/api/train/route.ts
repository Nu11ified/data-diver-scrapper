import { NextResponse } from "next/server";
import { withErrors } from "@/lib/api";
import { prisma } from "@/lib/db";
import { Prisma } from "@/generated/prisma/client";
import * as engine from "@/lib/engine";
import { syncFromEngine } from "@/lib/sync";
import { toDate } from "@/lib/format";

/** Retrain the classifier on the engine, persist the report, mirror. */
export async function POST() {
  return withErrors(async () => {
    const report = await engine.train();
    const stored = await prisma.trainingReport.create({
      data: {
        at: toDate(report.at) ?? new Date(),
        durationMs: report.duration_ms,
        examples: report.examples,
        classes: report.classes,
        accuracy: report.accuracy,
        perClass: report.per_class as unknown as Prisma.InputJsonValue,
        confusion: report.confusion as unknown as Prisma.InputJsonValue,
      },
    });
    await syncFromEngine();
    return NextResponse.json({ report, stored });
  });
}
