"use client";

import { useQuery } from "@tanstack/react-query";
import type { Overview } from "@/lib/engine";
import { fmtBytes, fmtMs } from "@/lib/format";
import { Skeleton } from "@/components/ui/skeleton";
import { cn } from "@/lib/utils";

async function fetchOverview(): Promise<Overview> {
  const res = await fetch("/api/overview");
  if (!res.ok) {
    const body = (await res.json().catch(() => null)) as {
      message?: string;
    } | null;
    throw new Error(body?.message ?? `overview failed (${res.status})`);
  }
  return (await res.json()) as Overview;
}

/**
 * Live engine pill: polls /api/overview and shows the engine's measured
 * RSS and CPU time, or an offline marker when the engine is unreachable.
 */
export function EngineStatus({ className }: { className?: string }) {
  const { data, isPending, isError } = useQuery({
    queryKey: ["overview"],
    queryFn: fetchOverview,
    refetchInterval: 5_000,
    retry: false,
  });

  if (isPending) {
    return <Skeleton className={cn("h-6 w-36 rounded-full", className)} />;
  }

  const up = !isError && data != null;
  return (
    <div
      className={cn(
        "inline-flex items-center gap-2 rounded-full border px-2.5 py-1 text-xs tabular-nums",
        up
          ? "border-teal-600/20 bg-teal-50 text-teal-900"
          : "border-red-600/20 bg-red-50 text-red-900",
        className,
      )}
      title={up ? "Engine reachable" : "Engine unreachable"}
    >
      <span
        className={cn(
          "size-1.5 rounded-full",
          up ? "bg-teal-600" : "bg-red-600",
        )}
      />
      {up ? (
        <span>
          engine {fmtBytes(data.engine.rss_bytes)} rss ·{" "}
          {fmtMs(data.engine.cpu_ms)} cpu
        </span>
      ) : (
        <span>engine offline</span>
      )}
    </div>
  );
}
