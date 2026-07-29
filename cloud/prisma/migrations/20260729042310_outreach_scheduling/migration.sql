-- AlterTable
ALTER TABLE "outreach" ADD COLUMN     "audience" TEXT NOT NULL DEFAULT 'unknown',
ADD COLUMN     "scheduledFor" TIMESTAMP(3);
