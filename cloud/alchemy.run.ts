import * as Alchemy from "alchemy";
import * as Cloudflare from "alchemy/Cloudflare";
import * as Effect from "effect/Effect";

import Scraper from "./src/worker.ts";
import { Bucket } from "./src/resources.ts";

export default Alchemy.Stack(
  "DataDiver",
  {
    providers: Cloudflare.providers(),
    state: Cloudflare.state(),
  },
  Effect.gen(function* () {
    const bucket = yield* Bucket;
    const worker = yield* Scraper;
    return {
      bucketName: bucket.bucketName,
      workerUrl: worker.url,
    };
  }),
);
