import { Prisma, type PrismaClient } from "@/generated/prisma/client";
import { prisma } from "@/lib/db";
import * as engine from "@/lib/engine";
import { toDate } from "@/lib/format";

/**
 * Mirror the engine's state into Postgres (docs/architecture.md "Sync layer").
 * Pulls GET /api/export once, plus GET /api/schema per source for snapshot
 * fields, and idempotently upserts counties, sources, runs, events,
 * properties and repairs. CanonicalRecord rows are replaced with the latest
 * snapshot's records. Safe to call when the engine has no data yet.
 */
export interface SyncStats {
  counties: number;
  sources: number;
  runs: number;
  events: number;
  properties: number;
  repairs: number;
  records: number;
}

function slugify(name: string): string {
  return name
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "");
}

function json(v: unknown): Prisma.InputJsonValue {
  return v as Prisma.InputJsonValue;
}

function jsonOrNull(
  v: unknown,
): Prisma.InputJsonValue | typeof Prisma.DbNull {
  return v == null ? Prisma.DbNull : json(v);
}

function date(t: engine.EngineTime | null | undefined): Date {
  return toDate(t ?? null) ?? new Date(0);
}

export async function syncFromEngine(
  db: PrismaClient = prisma,
): Promise<SyncStats> {
  const data = await engine.getExport();

  // --- Counties ------------------------------------------------------------
  const countyIdBySlug = new Map<string, string>();
  const slugByJurisdiction = new Map<string, string>();
  for (const c of data.counties) {
    const row = await db.county.upsert({
      where: { slug: c.slug },
      update: { name: c.jurisdiction },
      create: { name: c.jurisdiction, slug: c.slug },
    });
    countyIdBySlug.set(row.slug, row.id);
    slugByJurisdiction.set(c.jurisdiction, c.slug);
  }

  /** County id for a jurisdiction name, creating the county if the export's
   *  counties list did not include it (defensive; should not happen). */
  async function countyIdFor(jurisdiction: string): Promise<string> {
    const slug = slugByJurisdiction.get(jurisdiction) ?? slugify(jurisdiction);
    const known = countyIdBySlug.get(slug);
    if (known) return known;
    const row = await db.county.upsert({
      where: { slug },
      update: {},
      create: { name: jurisdiction, slug },
    });
    countyIdBySlug.set(slug, row.id);
    slugByJurisdiction.set(jurisdiction, slug);
    return row.id;
  }

  // --- Sources (+ per-source snapshot fields + canonical records) ----------
  const knownSourceIds = new Set<string>();
  let recordCount = 0;
  for (const s of data.sources) {
    let snapshot: engine.Snapshot | null = null;
    try {
      snapshot = (await engine.getSchema(s.id)).snapshot;
    } catch (err) {
      // A source with no snapshot yet must not fail the whole sync.
      if (!(err instanceof engine.EngineHttpError)) throw err;
    }

    const countyId = await countyIdFor(s.jurisdiction);
    const fields = {
      name: s.name,
      url: s.url,
      enabled: s.enabled,
      countyId,
      classification: s.classification ?? snapshot?.classification.label ?? null,
      mappingConfidence: s.mapping_confidence ?? snapshot?.mapping.confidence ?? null,
      baselineRate: s.baseline_rate,
      goodRuns: s.good_runs,
      mapping: jsonOrNull(snapshot?.mapping ?? s.mapping),
      labels: jsonOrNull(snapshot?.labels),
      rawSample: jsonOrNull(snapshot?.raw_sample),
      classificationDist: jsonOrNull(snapshot?.classification.distribution),
      fieldRates: jsonOrNull(snapshot?.field_rates),
      lastRunAt: toDate(s.last_run?.started_at ?? null),
    };
    await db.source.upsert({
      where: { id: s.id },
      update: fields,
      create: { id: s.id, ...fields },
    });
    knownSourceIds.add(s.id);

    // Replace canonical records with the latest snapshot's rows.
    await db.canonicalRecord.deleteMany({ where: { sourceId: s.id } });
    if (snapshot && snapshot.records.length > 0) {
      const created = await db.canonicalRecord.createMany({
        data: snapshot.records.map((r, position) => {
          const { _completeness, ...values } = r;
          return {
            sourceId: s.id,
            runId: snapshot.run_id,
            position,
            values: json(values),
            completeness: _completeness ?? 0,
          };
        }),
      });
      recordCount += created.count;
    }
  }

  // --- Runs (immutable measurements; insert the ones we don't have) --------
  const runs = data.runs.filter((r) => knownSourceIds.has(r.source_id));
  if (runs.length > 0) {
    await db.run.createMany({
      data: runs.map((r) => ({
        id: r.id,
        sourceId: r.source_id,
        startedAt: date(r.started_at),
        ok: r.ok,
        stage: r.stage,
        error: r.error || null,
        httpStatus: r.http_status ?? 0,
        bytes: r.bytes ?? 0,
        fetchMs: r.fetch_ms ?? 0,
        parseMs: r.parse_ms ?? 0,
        classifyMs: r.classify_ms ?? 0,
        mapMs: r.map_ms ?? 0,
        totalMs: r.total_ms ?? 0,
        format: r.format ?? "",
        classification: r.classification ?? "",
        classConfidence: r.class_confidence ?? 0,
        recordsExtracted: r.records_extracted ?? 0,
        eventsNew: r.events_new ?? 0,
        extractionRate: r.extraction_rate ?? 0,
        mappingConfidence: r.mapping_confidence ?? 0,
        driftDetected: r.drift_detected ?? false,
        repairAccepted: r.repair_accepted ?? false,
        rssBytes: r.rss_bytes ?? 0,
        cpuMs: r.cpu_ms ?? 0,
      })),
      skipDuplicates: true,
    });
  }

  // --- Properties ----------------------------------------------------------
  const knownPropertyKeys = new Set<string>();
  for (const p of data.properties) {
    const countyId =
      countyIdBySlug.get(p.jurisdiction_slug) ??
      (await countyIdFor(p.jurisdiction_slug));
    const fields = {
      countyId,
      parcelId: p.parcel_id ?? null,
      owner: p.owner ?? null,
      address: p.address ?? null,
      state: p.state,
      transitions: jsonOrNull(p.transitions),
      sourcesCount: p.sources?.length ?? 1,
      lastEventDate: p.last_event_date ?? null,
    };
    await db.property.upsert({
      where: { key: p.key },
      update: fields,
      create: { key: p.key, ...fields },
    });
    knownPropertyKeys.add(p.key);
  }

  // --- Events (content-hash ids, immutable) --------------------------------
  const events = data.events.filter((e) => knownPropertyKeys.has(e.property_key));
  if (events.length > 0) {
    await db.event.createMany({
      data: events.map((e) => ({
        id: e.id,
        propertyKey: e.property_key,
        sourceId:
          e.source_id && knownSourceIds.has(e.source_id) ? e.source_id : null,
        kind: e.kind,
        eventDate: e.event_date ?? null,
        recordedAt: date(e.recorded_at),
        runId: e.run_id,
        amount: e.amount ?? 0,
        confidence: e.confidence ?? 0,
        details: json(e.details ?? {}),
      })),
      skipDuplicates: true,
    });
  }

  // --- Repairs (resolution can change, so upsert) --------------------------
  const repairs = data.repairs.filter((r) => knownSourceIds.has(r.source_id));
  for (const r of repairs) {
    const fields = {
      sourceId: r.source_id,
      at: date(r.at),
      reason: r.reason,
      beforeMapping: jsonOrNull(r.before_mapping),
      afterMapping: jsonOrNull(r.after_mapping),
      beforeRate: r.before_rate ?? 0,
      afterRate: r.after_rate ?? 0,
      confidence: r.confidence ?? 0,
      // Legacy records without the field: accepted -> auto, else pending.
      resolution: r.resolution ?? (r.accepted ? "auto" : "pending"),
      changes: json(r.changes ?? []),
    };
    await db.repair.upsert({
      where: { id: r.id },
      update: fields,
      create: { id: r.id, ...fields },
    });
  }

  return {
    counties: data.counties.length,
    sources: data.sources.length,
    runs: runs.length,
    events: events.length,
    properties: data.properties.length,
    repairs: repairs.length,
    records: recordCount,
  };
}
