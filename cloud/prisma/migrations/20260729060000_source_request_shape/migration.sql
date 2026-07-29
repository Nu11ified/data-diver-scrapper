-- AlterTable
ALTER TABLE "sources" ADD COLUMN     "headers" JSONB,
ADD COLUMN     "method" TEXT,
ADD COLUMN     "body" TEXT;

-- AlterTable
ALTER TABLE "runs" ADD COLUMN     "truncated" BOOLEAN NOT NULL DEFAULT false;
