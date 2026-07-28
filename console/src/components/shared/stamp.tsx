import { cn } from "@/lib/utils";

export type StampTone = "teal" | "amber" | "red" | "neutral";

const TONES: Record<StampTone, string> = {
  teal: "bg-teal-50 text-teal-800 ring-teal-600/20",
  amber: "bg-amber-50 text-amber-800 ring-amber-600/25",
  red: "bg-red-50 text-red-800 ring-red-600/20",
  neutral: "bg-muted text-muted-foreground ring-border",
};

/**
 * Small status badge: teal = healthy/ok, amber = attention/pending,
 * red = failed/rejected, neutral = inactive/unknown.
 */
export function Stamp({
  tone = "neutral",
  children,
  className,
}: {
  tone?: StampTone;
  children: React.ReactNode;
  className?: string;
}) {
  return (
    <span
      className={cn(
        "inline-flex items-center gap-1 rounded-full px-2 py-0.5 text-xs font-medium ring-1 ring-inset",
        TONES[tone],
        className,
      )}
    >
      {children}
    </span>
  );
}
