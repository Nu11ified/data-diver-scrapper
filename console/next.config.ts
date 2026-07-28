import type { NextConfig } from "next";
import path from "node:path";

const nextConfig: NextConfig = {
  // A stray lockfile in a parent directory otherwise makes Turbopack guess
  // the wrong workspace root.
  turbopack: { root: path.join(import.meta.dirname) },
};

export default nextConfig;
