import { NextResponse } from "next/server";
import { withErrors } from "@/lib/api";
import * as engine from "@/lib/engine";

/** Live model internals, proxied from the engine (not persisted). */
export async function GET() {
  return withErrors(async () => {
    const model = await engine.getModel();
    return NextResponse.json(model);
  });
}
