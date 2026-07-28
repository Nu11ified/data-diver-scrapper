/** Formatting helpers shared by console pages and components. */

/** 1536 -> "1.5 KB". Accepts null/undefined and renders an em dash. */
export function fmtBytes(n: number | null | undefined): string {
  if (n == null || Number.isNaN(n)) return "—";
  if (n < 1024) return `${Math.round(n)} B`;
  const units = ["KB", "MB", "GB", "TB"];
  let v = n / 1024;
  let i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  return `${v >= 100 ? v.toFixed(0) : v.toFixed(1)} ${units[i]}`;
}

/** 12.4 -> "12 ms", 1830 -> "1.83 s". */
export function fmtMs(n: number | null | undefined): string {
  if (n == null || Number.isNaN(n)) return "—";
  if (n < 1) return `${n.toFixed(2)} ms`;
  if (n < 1000) return `${Math.round(n)} ms`;
  const s = n / 1000;
  if (s < 60) return `${s >= 10 ? s.toFixed(1) : s.toFixed(2)} s`;
  const m = Math.floor(s / 60);
  return `${m}m ${Math.round(s - m * 60)}s`;
}

/** Fraction 0..1 -> "94%". `digits` controls decimals. */
export function fmtPct(fraction: number | null | undefined, digits = 0): string {
  if (fraction == null || Number.isNaN(fraction)) return "—";
  return `${(fraction * 100).toFixed(digits)}%`;
}

/**
 * Engine timestamps arrive as ISO strings or unix epochs (seconds or ms).
 * Renders a compact local date-time, or "—" when absent/invalid.
 */
export function fmtTime(t: string | number | Date | null | undefined): string {
  const d = toDate(t);
  if (!d) return "—";
  return d.toLocaleString(undefined, {
    month: "short",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
  });
}

/** Best-effort conversion of an engine timestamp to a Date; null if invalid. */
export function toDate(
  t: string | number | Date | null | undefined,
): Date | null {
  if (t == null || t === "" || t === 0) return null;
  if (t instanceof Date) return Number.isNaN(t.getTime()) ? null : t;
  if (typeof t === "number") {
    // Heuristic: epoch seconds fit well under 1e12 until the year 33658.
    const ms = t < 1e12 ? t * 1000 : t;
    const d = new Date(ms);
    return Number.isNaN(d.getTime()) ? null : d;
  }
  const asNum = Number(t);
  if (t.trim() !== "" && !Number.isNaN(asNum)) return toDate(asNum);
  const d = new Date(t);
  return Number.isNaN(d.getTime()) ? null : d;
}
