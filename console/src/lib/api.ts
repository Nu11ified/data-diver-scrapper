import { NextResponse } from "next/server";
import { EngineDownError, EngineHttpError } from "@/lib/engine";

/**
 * Run a route handler body and translate failures into clear JSON errors:
 * engine unreachable -> 502, engine rejected the call -> its status, anything
 * else -> 500. Keeps individual route handlers thin.
 */
export async function withErrors(
  fn: () => Promise<NextResponse>,
): Promise<NextResponse> {
  try {
    return await fn();
  } catch (err) {
    if (err instanceof EngineDownError) {
      return NextResponse.json(
        { error: "engine_unreachable", message: err.message },
        { status: 502 },
      );
    }
    if (err instanceof EngineHttpError) {
      return NextResponse.json(
        { error: "engine_error", status: err.status, message: err.message },
        { status: err.status >= 400 && err.status < 600 ? err.status : 502 },
      );
    }
    const message = err instanceof Error ? err.message : String(err);
    return NextResponse.json(
      { error: "internal_error", message },
      { status: 500 },
    );
  }
}
