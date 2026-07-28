import { NextResponse } from "next/server";
import { withErrors } from "@/lib/api";
import { prisma } from "@/lib/db";
import * as engine from "@/lib/engine";

/**
 * Live engine overview plus Postgres totals. Each successful poll also
 * records an EngineSample row (real measurements from the engine's OS
 * counters) for the performance page.
 */
export async function GET() {
  return withErrors(async () => {
    const overview = await engine.getOverview();
    const [counties, sources, runs, events, properties, repairs] =
      await Promise.all([
        prisma.county.count(),
        prisma.source.count(),
        prisma.run.count(),
        prisma.event.count(),
        prisma.property.count(),
        prisma.repair.count(),
      ]);
    await prisma.engineSample.create({
      data: {
        rssBytes: overview.engine.rss_bytes,
        peakRss: overview.engine.peak_rss_bytes,
        cpuMs: overview.engine.cpu_ms,
        totalRuns: overview.totals.runs,
        totalEvents: overview.totals.events,
      },
    });
    return NextResponse.json({
      ...overview,
      db: { counties, sources, runs, events, properties, repairs },
    });
  });
}
