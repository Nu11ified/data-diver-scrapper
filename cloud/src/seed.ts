import { parseSeed, type SeedParse } from "./sources.ts";

/// The only reference to sources.json in the build; nothing on the default path reads it.
import bundled from "./engine/config/sources.json";

export const bundledSeed: SeedParse = parseSeed(bundled);
