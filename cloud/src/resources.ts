// Stack resources, declared once and imported by both the stack entry and
// the worker so bindings have a single source of truth and no import cycle.
import * as Cloudflare from "alchemy/Cloudflare";

export const Bucket = Cloudflare.R2.Bucket("Bucket");
