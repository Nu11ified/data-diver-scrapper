import { NextResponse } from "next/server";
import { withErrors } from "@/lib/api";
import { syncFromEngine } from "@/lib/sync";

/** Pull GET /api/export from the engine and mirror it into Postgres. */
export async function POST() {
  return withErrors(async () => {
    const stats = await syncFromEngine();
    return NextResponse.json({ ok: true, synced: stats });
  });
}
