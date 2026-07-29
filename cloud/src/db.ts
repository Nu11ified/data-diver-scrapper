import { PrismaNeon } from "@prisma/adapter-neon";
import { PrismaClient } from "./generated/prisma/client.ts";

export const makeClient = (connectionString: string): PrismaClient =>
  new PrismaClient({ adapter: new PrismaNeon({ connectionString }) });

export type Db = PrismaClient;
