import * as Effect from "effect/Effect";

import { makeLimiter, type Limiter } from "./politeness.ts";

/// The engine's crawler lives in C++, but wasm in a Worker has no sockets, so
/// the fetching half is here and the parsing half stays in the module.

export interface CrawlPage {
  readonly url: string;
  readonly contentType: string;
  readonly body: string;
}

export interface CrawlOptions {
  readonly maxPages: number;
  readonly maxDepth: number;
  readonly userAgent: string;
  /// Shared so one request cannot spend several crawls' worth of a host's
  /// budget by fanning out over candidates.
  readonly limiter?: Limiter;
}

const LINK = /<a\b[^>]*?\shref\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+))/gi;

export const resolveUrl = (base: string, href: string): string => {
  const trimmed = href.trim();
  if (trimmed === "" || trimmed.startsWith("#")) return "";
  if (/^(mailto|javascript|tel|data|ftp):/i.test(trimmed)) return "";
  try {
    const resolved = new URL(trimmed, base);
    if (resolved.protocol !== "https:" && resolved.protocol !== "http:") return "";
    resolved.hash = "";
    return resolved.toString();
  } catch {
    return "";
  }
};

export const linksFrom = (url: string, html: string, limit: number): readonly string[] => {
  const paginated: string[] = [];
  const rest: string[] = [];
  const seen = new Set<string>();
  for (const match of html.matchAll(LINK)) {
    const href = match[1] ?? match[2] ?? match[3] ?? "";
    const resolved = resolveUrl(url, href);
    if (resolved === "" || seen.has(resolved)) continue;
    seen.add(resolved);
    // A results table is usually spread over pages, and the record links
    // repeat on every one of them, so pagination goes first. The window stops
    // at this anchor's own close tag: run past it and every link inherits the
    // "next" of whichever anchor happens to follow it.
    const from = match.index ?? 0;
    const close = html.indexOf("</a", from);
    const anchor = html
      .slice(from, close === -1 ? Math.min(from + 200, html.length) : close + 3)
      .toLowerCase();
    (/>\s*(next|next page|»|&raquo;|\d{1,3})\s*(<|$)/.test(anchor) ||
    /rel\s*=\s*["']?next/.test(anchor)
      ? paginated
      : rest
    ).push(resolved);
  }
  return [...paginated, ...rest].slice(0, limit);
};

/// The subset of robots.txt that decides whether a path may be fetched.
export const disallowedPaths = (robots: string): readonly string[] => {
  const out: string[] = [];
  let applies = false;
  for (const raw of robots.split("\n")) {
    const line = raw.split("#")[0]?.trim() ?? "";
    const colon = line.indexOf(":");
    if (colon === -1) continue;
    const key = line.slice(0, colon).trim().toLowerCase();
    const value = line.slice(colon + 1).trim();
    if (key === "user-agent") {
      applies = value === "*";
      continue;
    }
    if (key === "disallow" && applies && value !== "") out.push(value);
  }
  return out;
};

export const crawl = <E, R>(
  seed: string,
  options: CrawlOptions,
  visit: (page: CrawlPage) => Effect.Effect<boolean, E, R>,
): Effect.Effect<{ readonly fetched: number; readonly blocked: number }, E, R> =>
  Effect.gen(function* () {
    let origin: string;
    let host: string;
    try {
      const parsed = new URL(seed);
      origin = parsed.origin;
      host = parsed.host;
    } catch {
      return { fetched: 0, blocked: 0 };
    }

    const headers = { "user-agent": options.userAgent };
    const robots = yield* Effect.tryPromise({
      try: () => fetch(`${origin}/robots.txt`, { headers }).then((r) => (r.ok ? r.text() : "")),
      catch: () => new Error("no robots"),
    }).pipe(Effect.catch(() => Effect.succeed("")));
    const denied = disallowedPaths(robots);
    const limiter = options.limiter ?? makeLimiter();

    const queue: Array<{ url: string; depth: number }> = [{ url: seed, depth: 0 }];
    const queued = new Set([seed]);
    const bodies = new Set<string>();
    let fetched = 0;
    let blocked = 0;

    while (queue.length > 0 && fetched < options.maxPages) {
      const next = queue.shift();
      if (next === undefined) break;
      const parsed = new URL(next.url);
      if (parsed.host !== host) continue;
      if (denied.some((prefix) => parsed.pathname.startsWith(prefix))) {
        blocked += 1;
        continue;
      }

      const allowed = yield* Effect.promise(() => limiter.take(next.url));
      if (!allowed) {
        blocked += 1;
        continue;
      }
      const response = yield* Effect.tryPromise({
        try: () => fetch(next.url, { headers }),
        catch: (cause): Error => (cause instanceof Error ? cause : new Error(String(cause))),
      }).pipe(Effect.catch(() => Effect.succeed(undefined)));
      if (response === undefined || !response.ok) continue;
      const contentType = response.headers.get("content-type") ?? "";
      const body = yield* Effect.promise(() => response.text());
      // The same table under two urls is one page.
      const digest = `${body.length}:${body.slice(0, 200)}`;
      if (bodies.has(digest)) continue;
      bodies.add(digest);
      fetched += 1;

      const keepGoing = yield* visit({ url: next.url, contentType, body });
      if (!keepGoing) break;
      if (next.depth >= options.maxDepth) continue;
      if (contentType !== "" && !contentType.toLowerCase().includes("html")) continue;
      for (const link of linksFrom(next.url, body, 60)) {
        if (queued.size >= options.maxPages * 6) break;
        if (new URL(link).host !== host) continue;
        if (!queued.has(link)) {
          queued.add(link);
          queue.push({ url: link, depth: next.depth + 1 });
        }
      }
    }
    return { fetched, blocked };
  });
