import * as Alchemy from "alchemy";
import * as Cloudflare from "alchemy/Cloudflare";
import * as Effect from "effect/Effect";

// Resources are declared at module scope so the Worker's env type is a
// single source of truth; the Stack program below provisions them.
const Bucket = Cloudflare.R2.Bucket("Bucket");

export const Worker = Cloudflare.Worker("Scraper", {
  main: "./src/worker.ts",
  env: { bucket: Bucket },
  crons: ["0 * * * *"],
});

export type WorkerEnv = Cloudflare.InferEnv<typeof Worker>;

export default Alchemy.Stack(
  "GoliathScout",
  {
    providers: Cloudflare.providers(),
    state: Cloudflare.state(),
  },
  Effect.gen(function* () {
    const bucket = yield* Bucket;
    const worker = yield* Worker;
    return {
      bucketName: bucket.bucketName,
      workerUrl: worker.url,
    };
  }),
);
