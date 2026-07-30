export interface SourceConfig {
  readonly id: string;
  readonly name: string;
  readonly url: string;
  readonly jurisdiction: string;
  readonly as_of?: string;
  readonly headers?: Readonly<Record<string, string>>;
  readonly method?: "POST";
  readonly body?: string;
}

export interface SourceRow {
  readonly id: string;
  readonly name: string;
  readonly url: string;
  readonly jurisdiction: string;
  readonly asOf: string | null;
  readonly headers: unknown;
  readonly method: string | null;
  readonly body: string | null;
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

export const marketFromJurisdiction = (raw: string): string => {
  const existing = /^(.+?),\s*([a-z]{2})$/i.exec(raw.trim());
  if (existing !== null) {
    return `${existing[1]?.trim() ?? ""}, ${(existing[2] ?? "").toUpperCase()}`;
  }
  const words = slugId(raw).split("_").filter((word) => word !== "");
  const state = words.at(-1);
  if (state?.length === 2) words.pop();
  const place = words
    .map((word, index) =>
      word === "of" && index > 0
        ? word
        : `${word.slice(0, 1).toUpperCase()}${word.slice(1)}`,
    )
    .join(" ");
  return state?.length === 2 && place !== ""
    ? `${place}, ${state.toUpperCase()}`
    : place;
};

const stringHeaders = (value: unknown): Readonly<Record<string, string>> | undefined => {
  if (typeof value !== "object" || value === null || Array.isArray(value)) return undefined;
  const out: Record<string, string> = {};
  for (const [key, entry] of Object.entries(value as Record<string, unknown>)) {
    if (typeof entry === "string") out[key] = entry;
  }
  return Object.keys(out).length === 0 ? undefined : out;
};

export const configFromRow = (row: SourceRow): SourceConfig => {
  const headers = stringHeaders(row.headers);
  return {
    id: row.id,
    name: row.name,
    url: row.url,
    jurisdiction: row.jurisdiction,
    ...(row.asOf === null || row.asOf === "" ? {} : { as_of: row.asOf }),
    ...(headers === undefined ? {} : { headers }),
    ...(row.method === "POST" ? { method: "POST" as const } : {}),
    ...(row.body === null || row.body === "" ? {} : { body: row.body }),
  };
};

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
  const rawHeaders = fields["headers"];
  if (
    rawHeaders !== undefined &&
    (typeof rawHeaders !== "object" || rawHeaders === null || Array.isArray(rawHeaders))
  ) {
    return { reason: `${id}: headers must be an object of strings` };
  }
  const headers: Record<string, string> = {};
  if (rawHeaders !== undefined) {
    for (const [key, value] of Object.entries(rawHeaders as Readonly<Record<string, unknown>>)) {
      if (typeof value !== "string") return { reason: `${id}: headers must be an object of strings` };
      headers[key] = value;
    }
  }
  const methodRaw = text(fields["method"]).toUpperCase();
  if (methodRaw !== "" && methodRaw !== "GET" && methodRaw !== "POST") {
    return { reason: `${id}: method must be GET or POST` };
  }
  const body = text(fields["body"]);
  return {
    source: {
      id,
      name,
      url,
      jurisdiction,
      ...(asOf === "" ? {} : { as_of: asOf }),
      ...(Object.keys(headers).length === 0 ? {} : { headers }),
      ...(methodRaw === "POST" ? { method: "POST" as const } : {}),
      ...(body === "" ? {} : { body }),
    },
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
