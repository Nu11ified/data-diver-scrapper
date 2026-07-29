export interface SourceConfig {
  readonly id: string;
  readonly name: string;
  readonly url: string;
  readonly jurisdiction: string;
  readonly as_of?: string;
}

export interface SourceRow {
  readonly id: string;
  readonly name: string;
  readonly url: string;
  readonly jurisdiction: string;
  readonly asOf: string | null;
}

export interface SeedParse {
  readonly sources: readonly SourceConfig[];
  readonly rejected: readonly string[];
}

export interface SeedPlan {
  readonly create: readonly SourceConfig[];
  readonly skipped: readonly string[];
}

export const slugId = (raw: string): string =>
  raw.toLowerCase().replace(/[^a-z0-9_]/g, "_").replace(/^_+|_+$/g, "");

export const configFromRow = (row: SourceRow): SourceConfig => ({
  id: row.id,
  name: row.name,
  url: row.url,
  jurisdiction: row.jurisdiction,
  ...(row.asOf === null || row.asOf === "" ? {} : { as_of: row.asOf }),
});

const text = (value: unknown): string => (typeof value === "string" ? value.trim() : "");

const readEntry = (
  entry: unknown,
  label: string,
): { readonly source: SourceConfig } | { readonly reason: string } => {
  if (typeof entry !== "object" || entry === null || Array.isArray(entry)) {
    return { reason: `${label}: not an object` };
  }
  const fields = entry as Readonly<Record<string, unknown>>;
  const id = slugId(text(fields["id"]));
  const name = text(fields["name"]);
  const url = text(fields["url"]);
  const jurisdiction = text(fields["jurisdiction"]);
  const asOf = text(fields["as_of"]);
  if (id === "") return { reason: `${label}: id is missing` };
  if (name === "") return { reason: `${id}: name is missing` };
  if (jurisdiction === "") return { reason: `${id}: jurisdiction is missing` };
  if (!url.startsWith("https://")) return { reason: `${id}: url is not https` };
  return {
    source: { id, name, url, jurisdiction, ...(asOf === "" ? {} : { as_of: asOf }) },
  };
};

export const parseSeed = (raw: unknown): SeedParse => {
  if (!Array.isArray(raw)) return { sources: [], rejected: ["seed is not an array"] };
  const sources: SourceConfig[] = [];
  const rejected: string[] = [];
  raw.forEach((entry: unknown, index) => {
    const read = readEntry(entry, `entry ${index}`);
    if ("reason" in read) rejected.push(read.reason);
    else sources.push(read.source);
  });
  return { sources, rejected };
};

export const planSeed = (
  seed: readonly SourceConfig[],
  existingIds: Iterable<string>,
): SeedPlan => {
  const seen = new Set(existingIds);
  const create: SourceConfig[] = [];
  const skipped: string[] = [];
  for (const source of seed) {
    if (seen.has(source.id)) {
      skipped.push(source.id);
      continue;
    }
    seen.add(source.id);
    create.push(source);
  }
  return { create, skipped };
};
