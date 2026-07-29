export type Audience = "business" | "person" | "unknown";

const BUSINESS_TOKENS = [
  "llc",
  "inc",
  "corp",
  "corporation",
  "company",
  "co",
  "ltd",
  "lp",
  "llp",
  "trust",
  "trustee",
  "trustees",
  "holdings",
  "properties",
  "investments",
  "enterprises",
  "partners",
  "partnership",
  "bank",
  "association",
  "hoa",
  "church",
  "ministries",
  "authority",
  "realty",
  "development",
  "group",
  "city of",
  "county of",
  "commonwealth of",
  "school board",
];

export const classifyOwner = (owner: string): Audience => {
  const normalized = ` ${owner.toLowerCase().replace(/[.,&()]/g, " ").replace(/\s+/g, " ").trim()} `;
  if (normalized.trim() === "") return "unknown";
  if (normalized.includes(" estate of ")) return "person";
  for (const token of BUSINESS_TOKENS) {
    if (normalized.includes(` ${token} `)) return "business";
  }
  return "person";
};

const ZONE_BY_STATE: Readonly<Record<string, string>> = {
  va: "America/New_York",
  nc: "America/New_York",
  md: "America/New_York",
  dc: "America/New_York",
  fl: "America/New_York",
  ga: "America/New_York",
  tx: "America/Chicago",
  il: "America/Chicago",
  co: "America/Denver",
  az: "America/Phoenix",
  ca: "America/Los_Angeles",
  wa: "America/Los_Angeles",
  or: "America/Los_Angeles",
};

export const zoneFor = (jurisdiction: string): string => {
  const state = /_([a-z]{2})$/.exec(jurisdiction)?.[1] ?? "";
  return ZONE_BY_STATE[state] ?? "America/New_York";
};

const localParts = (date: Date, timeZone: string): { day: number; minutes: number } => {
  const parts = new Intl.DateTimeFormat("en-US", {
    timeZone,
    weekday: "short",
    hour: "2-digit",
    minute: "2-digit",
    hour12: false,
  }).formatToParts(date);
  const get = (type: string): string => parts.find((p) => p.type === type)?.value ?? "";
  const day = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"].indexOf(get("weekday"));
  // hour12:false can render midnight as "24"
  const hour = Number.parseInt(get("hour"), 10) % 24;
  return { day, minutes: hour * 60 + Number.parseInt(get("minute"), 10) };
};

interface Window {
  readonly start: number;
  readonly end: number;
  readonly reason: string;
}

const windowsFor = (audience: Audience, day: number): readonly Window[] => {
  const weekday = day >= 1 && day <= 5;
  if (audience === "business") {
    return weekday
      ? [{ start: 9 * 60, end: 17 * 60, reason: "business hours for a business owner" }]
      : [];
  }
  return weekday
    ? [{ start: 17 * 60 + 30, end: 20 * 60 + 30, reason: "after-work hours for an individual owner" }]
    : [{ start: 10 * 60, end: 20 * 60 + 30, reason: "weekend daytime for an individual owner" }];
};

export interface SendWindow {
  readonly at: Date;
  readonly reason: string;
}

export const nextSendWindow = (
  audience: Audience,
  now: Date,
  timeZone: string,
): SendWindow => {
  const local = localParts(now, timeZone);
  for (let offset = 0; offset <= 7; offset += 1) {
    const day = (local.day + offset) % 7;
    for (const window of windowsFor(audience, day)) {
      if (offset === 0 && local.minutes >= window.start && local.minutes < window.end) {
        return { at: now, reason: window.reason };
      }
      const delta = offset * 1_440 + window.start - local.minutes;
      if (delta > 0) {
        return { at: new Date(now.getTime() + delta * 60_000), reason: window.reason };
      }
    }
  }
  return { at: now, reason: "no window found; sending immediately" };
};

export const formatInZone = (date: Date, timeZone: string): string =>
  new Intl.DateTimeFormat("en-US", {
    timeZone,
    weekday: "short",
    month: "short",
    day: "numeric",
    hour: "numeric",
    minute: "2-digit",
    timeZoneName: "short",
  }).format(date);
