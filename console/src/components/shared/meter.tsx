import { Progress } from "@/components/ui/progress";
import { fmtPct } from "@/lib/format";
import { cn } from "@/lib/utils";

/**
 * Small horizontal meter with a % readout. `value` is a fraction 0..1
 * (e.g. an extraction rate or a confidence) measured during a run.
 */
export function Meter({
  value,
  className,
}: {
  value: number | null | undefined;
  className?: string;
}) {
  const pct =
    value == null || Number.isNaN(value)
      ? null
      : Math.max(0, Math.min(1, value)) * 100;
  return (
    <div className={cn("flex items-center gap-2", className)}>
      <Progress value={pct ?? 0} className="h-1.5 w-20" />
      <span className="w-10 text-right text-xs tabular-nums text-muted-foreground">
        {pct == null ? "—" : fmtPct(pct / 100)}
      </span>
    </div>
  );
}
