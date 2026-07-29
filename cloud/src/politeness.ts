/// A budget on how hard this worker may hit one host.
///
/// Every fetch path here points at someone else's county server, and the
/// endpoints that trigger them are open. Without a ceiling a single caller can
/// turn the worker into a battering ram against a government site, which is
/// both rude and the fastest way to be blocked.

export interface HostBudget {
  readonly perHost: number;
  readonly windowMs: number;
  readonly minGapMs: number;
}

export const DEFAULT_BUDGET: HostBudget = {
  perHost: 60,
  windowMs: 60_000,
  minGapMs: 250,
};

export interface Limiter {
  /// Waits for this host's turn. Resolves false when the host's budget for the
  /// window is spent, which the caller should treat as "stop", not "retry".
  readonly take: (url: string) => Promise<boolean>;
  readonly spent: (host: string) => number;
}

const hostOf = (url: string): string => {
  try {
    return new URL(url).host.toLowerCase();
  } catch {
    return "";
  }
};

export const makeLimiter = (
  budget: HostBudget = DEFAULT_BUDGET,
  now: () => number = () => Date.now(),
  sleep: (ms: number) => Promise<void> = (ms) =>
    new Promise((resolve) => setTimeout(resolve, ms)),
): Limiter => {
  const windowStart = new Map<string, number>();
  const used = new Map<string, number>();
  const lastAt = new Map<string, number>();

  return {
    take: async (url) => {
      const host = hostOf(url);
      if (host === "") return false;
      const at = now();
      const started = windowStart.get(host) ?? 0;
      if (at - started >= budget.windowMs) {
        windowStart.set(host, at);
        used.set(host, 0);
      }
      const count = used.get(host) ?? 0;
      if (count >= budget.perHost) return false;
      used.set(host, count + 1);

      const previous = lastAt.get(host);
      if (previous !== undefined) {
        const gap = at - previous;
        if (gap < budget.minGapMs) await sleep(budget.minGapMs - gap);
      }
      lastAt.set(host, now());
      return true;
    },
    spent: (host) => used.get(host.toLowerCase()) ?? 0,
  };
};
