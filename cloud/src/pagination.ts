import type { Limiter } from "./politeness.ts";

/// Same marker the engine's harvester uses to recognise a Socrata dataset endpoint.
export const SOCRATA_MARKER = "/resource/";

export const RECORD_CAP = 5000;

export const isSocrataUrl = (url: string): boolean => url.includes(SOCRATA_MARKER);

export const socrataLimit = (url: string): number => {
  try {
    const raw = Number(new URL(url).searchParams.get("$limit"));
    return Number.isFinite(raw) && raw > 0 ? Math.floor(raw) : 1000;
  } catch {
    return 1000;
  }
};

export const withOffset = (url: string, limit: number, offset: number): string => {
  const next = new URL(url);
  next.searchParams.set("$limit", String(limit));
  next.searchParams.set("$offset", String(offset));
  return next.toString();
};

export const appTokenHeaders = (url: string, token: string): Readonly<Record<string, string>> =>
  token === "" || !isSocrataUrl(url) ? {} : { "X-App-Token": token };

export interface PageFetch {
  readonly ok: boolean;
  readonly status: number;
  readonly contentType: string;
  readonly body: string;
}

export interface FetchResult {
  readonly ok: boolean;
  readonly body: string;
  readonly contentType: string;
  readonly truncated: boolean;
  readonly pages: number;
  readonly error?: string;
}

const fetchError = (message: string): FetchResult => ({
  ok: false,
  body: "",
  contentType: "",
  truncated: false,
  pages: 0,
  error: message,
});

export const fetchSingle = async (
  url: string,
  limiter: Limiter,
  fetchPage: (pageUrl: string) => Promise<PageFetch>,
): Promise<FetchResult> => {
  const permitted = await limiter.take(url);
  if (!permitted) return fetchError("host request budget spent; try again shortly");
  try {
    const page = await fetchPage(url);
    if (!page.ok) return fetchError(`http status ${page.status}`);
    return { ok: true, body: page.body, contentType: page.contentType, truncated: false, pages: 1 };
  } catch (cause) {
    return fetchError(cause instanceof Error ? cause.message : String(cause));
  }
};

export const fetchPaginated = async (
  url: string,
  limiter: Limiter,
  fetchPage: (pageUrl: string) => Promise<PageFetch>,
  cap: number = RECORD_CAP,
  minimumPageSize = 0,
): Promise<FetchResult> => {
  const requestedLimit = socrataLimit(url);
  const limit = Math.max(requestedLimit, minimumPageSize);
  const records: unknown[] = [];
  let contentType = "";
  let pages = 0;
  let offset = 0;

  for (;;) {
    const pageUrl =
      pages === 0 && limit === requestedLimit
        ? url
        : withOffset(url, limit, offset);
    const permitted = await limiter.take(pageUrl);
    if (!permitted) {
      if (pages === 0) return fetchError("host request budget spent; try again shortly");
      return { ok: true, body: JSON.stringify(records), contentType, truncated: true, pages };
    }

    let page: PageFetch;
    try {
      page = await fetchPage(pageUrl);
    } catch (cause) {
      const message = cause instanceof Error ? cause.message : String(cause);
      if (pages === 0) return fetchError(message);
      return { ok: true, body: JSON.stringify(records), contentType, truncated: true, pages };
    }
    if (!page.ok) {
      if (pages === 0) return fetchError(`http status ${page.status}`);
      return { ok: true, body: JSON.stringify(records), contentType, truncated: true, pages };
    }

    pages += 1;
    contentType = page.contentType;
    let parsed: unknown;
    try {
      parsed = JSON.parse(page.body);
    } catch {
      parsed = undefined;
    }
    if (!Array.isArray(parsed)) {
      if (pages === 1) return fetchError("expected a json array of records");
      return { ok: true, body: JSON.stringify(records), contentType, truncated: true, pages };
    }

    if (records.length + parsed.length > cap) {
      records.push(...parsed.slice(0, cap - records.length));
      return { ok: true, body: JSON.stringify(records), contentType, truncated: true, pages };
    }
    records.push(...parsed);
    if (parsed.length < limit) break;
    if (records.length >= cap) {
      return { ok: true, body: JSON.stringify(records), contentType, truncated: true, pages };
    }
    offset += limit;
  }
  return { ok: true, body: JSON.stringify(records), contentType, truncated: false, pages };
};
