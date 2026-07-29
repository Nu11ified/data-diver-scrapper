-- CreateTable
CREATE TABLE "tenants" (
    "id" UUID NOT NULL,
    "phone" TEXT NOT NULL,
    "createdAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,

    CONSTRAINT "tenants_pkey" PRIMARY KEY ("id")
);

-- CreateTable
CREATE TABLE "credentials" (
    "id" UUID NOT NULL,
    "tenantId" UUID NOT NULL,
    "provider" TEXT NOT NULL,
    "ciphertext" BYTEA NOT NULL,
    "iv" BYTEA NOT NULL,
    "wrappedKey" BYTEA NOT NULL,
    "accountLabel" TEXT,
    "expiresAt" TIMESTAMP(3),
    "createdAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "updatedAt" TIMESTAMP(3) NOT NULL,

    CONSTRAINT "credentials_pkey" PRIMARY KEY ("id")
);

-- CreateTable
CREATE TABLE "criteria" (
    "id" UUID NOT NULL,
    "tenantId" UUID NOT NULL,
    "minOwed" DECIMAL(14,2) NOT NULL,
    "requireMultiSource" BOOLEAN NOT NULL DEFAULT false,
    "minDebtToValue" DECIMAL(6,3) NOT NULL DEFAULT 0,
    "updatedAt" TIMESTAMP(3) NOT NULL,

    CONSTRAINT "criteria_pkey" PRIMARY KEY ("id")
);

-- CreateTable
CREATE TABLE "sources" (
    "id" TEXT NOT NULL,
    "name" TEXT NOT NULL,
    "url" TEXT NOT NULL,
    "jurisdiction" TEXT NOT NULL,
    "asOf" TEXT,
    "enabled" BOOLEAN NOT NULL DEFAULT true,
    "createdAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,

    CONSTRAINT "sources_pkey" PRIMARY KEY ("id")
);

-- CreateTable
CREATE TABLE "runs" (
    "id" TEXT NOT NULL,
    "sourceId" TEXT NOT NULL,
    "startedAt" TIMESTAMP(3) NOT NULL,
    "ok" BOOLEAN NOT NULL,
    "stage" TEXT NOT NULL,
    "error" TEXT,
    "classification" TEXT,
    "classConfidence" DOUBLE PRECISION,
    "extractionRate" DOUBLE PRECISION,
    "records" INTEGER NOT NULL DEFAULT 0,
    "fingerprint" TEXT,
    "newestRecordDate" TEXT,
    "fetchMs" INTEGER,
    "engineMs" INTEGER,
    "mapping" JSONB,

    CONSTRAINT "runs_pkey" PRIMARY KEY ("id")
);

-- CreateTable
CREATE TABLE "events" (
    "id" TEXT NOT NULL,
    "propertyKey" TEXT NOT NULL,
    "sourceId" TEXT NOT NULL,
    "runId" TEXT NOT NULL,
    "kind" TEXT NOT NULL,
    "eventDate" TEXT,
    "recordedAt" TIMESTAMP(3) NOT NULL,
    "asOf" TEXT,
    "amount" DECIMAL(16,2),
    "confidence" DOUBLE PRECISION NOT NULL,
    "details" JSONB NOT NULL,

    CONSTRAINT "events_pkey" PRIMARY KEY ("id")
);

-- CreateTable
CREATE TABLE "properties" (
    "key" TEXT NOT NULL,
    "jurisdiction" TEXT NOT NULL,
    "address" TEXT,
    "owner" TEXT,
    "parcelId" TEXT,
    "lifecycleState" TEXT NOT NULL,
    "owed" DECIMAL(16,2),
    "assessed" DECIMAL(16,2),
    "assessedPrior" DECIMAL(16,2),
    "debtToValue" DOUBLE PRECISION,
    "violations" INTEGER NOT NULL DEFAULT 0,
    "sourceCount" INTEGER NOT NULL DEFAULT 1,
    "mergedKeys" TEXT[],
    "fields" JSONB NOT NULL,
    "conflicts" JSONB NOT NULL,
    "compiledAt" TIMESTAMP(3) NOT NULL,

    CONSTRAINT "properties_pkey" PRIMARY KEY ("key")
);

-- CreateTable
CREATE TABLE "decision_trees" (
    "id" UUID NOT NULL,
    "tenantId" UUID NOT NULL,
    "name" TEXT NOT NULL,
    "version" INTEGER NOT NULL,
    "status" TEXT NOT NULL,
    "configuration" JSONB NOT NULL,
    "createdAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,

    CONSTRAINT "decision_trees_pkey" PRIMARY KEY ("id")
);

-- CreateTable
CREATE TABLE "evaluations" (
    "id" UUID NOT NULL,
    "tenantId" UUID NOT NULL,
    "propertyKey" TEXT NOT NULL,
    "decisionTreeId" UUID,
    "treeVersion" INTEGER,
    "outcome" TEXT NOT NULL,
    "trace" JSONB NOT NULL,
    "evaluatedAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,

    CONSTRAINT "evaluations_pkey" PRIMARY KEY ("id")
);

-- CreateTable
CREATE TABLE "outreach" (
    "id" UUID NOT NULL,
    "tenantId" UUID NOT NULL,
    "propertyKey" TEXT NOT NULL,
    "draft" TEXT NOT NULL,
    "status" TEXT NOT NULL,
    "approvedAt" TIMESTAMP(3),
    "sentAt" TIMESTAMP(3),
    "simulated" BOOLEAN NOT NULL DEFAULT true,
    "createdAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,

    CONSTRAINT "outreach_pkey" PRIMARY KEY ("id")
);

-- CreateTable
CREATE TABLE "messages" (
    "id" UUID NOT NULL,
    "tenantId" UUID NOT NULL,
    "role" TEXT NOT NULL,
    "body" TEXT NOT NULL,
    "at" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,

    CONSTRAINT "messages_pkey" PRIMARY KEY ("id")
);

-- CreateIndex
CREATE UNIQUE INDEX "tenants_phone_key" ON "tenants"("phone");

-- CreateIndex
CREATE UNIQUE INDEX "credentials_tenantId_key" ON "credentials"("tenantId");

-- CreateIndex
CREATE UNIQUE INDEX "criteria_tenantId_key" ON "criteria"("tenantId");

-- CreateIndex
CREATE INDEX "sources_jurisdiction_idx" ON "sources"("jurisdiction");

-- CreateIndex
CREATE INDEX "runs_sourceId_startedAt_idx" ON "runs"("sourceId", "startedAt");

-- CreateIndex
CREATE INDEX "events_propertyKey_eventDate_idx" ON "events"("propertyKey", "eventDate");

-- CreateIndex
CREATE INDEX "events_sourceId_idx" ON "events"("sourceId");

-- CreateIndex
CREATE INDEX "properties_jurisdiction_owed_idx" ON "properties"("jurisdiction", "owed");

-- CreateIndex
CREATE UNIQUE INDEX "decision_trees_tenantId_name_version_key" ON "decision_trees"("tenantId", "name", "version");

-- CreateIndex
CREATE INDEX "evaluations_tenantId_evaluatedAt_idx" ON "evaluations"("tenantId", "evaluatedAt");

-- CreateIndex
CREATE INDEX "outreach_tenantId_status_idx" ON "outreach"("tenantId", "status");

-- CreateIndex
CREATE INDEX "messages_tenantId_at_idx" ON "messages"("tenantId", "at");

-- AddForeignKey
ALTER TABLE "credentials" ADD CONSTRAINT "credentials_tenantId_fkey" FOREIGN KEY ("tenantId") REFERENCES "tenants"("id") ON DELETE CASCADE ON UPDATE CASCADE;

-- AddForeignKey
ALTER TABLE "criteria" ADD CONSTRAINT "criteria_tenantId_fkey" FOREIGN KEY ("tenantId") REFERENCES "tenants"("id") ON DELETE CASCADE ON UPDATE CASCADE;

-- AddForeignKey
ALTER TABLE "runs" ADD CONSTRAINT "runs_sourceId_fkey" FOREIGN KEY ("sourceId") REFERENCES "sources"("id") ON DELETE CASCADE ON UPDATE CASCADE;

-- AddForeignKey
ALTER TABLE "events" ADD CONSTRAINT "events_sourceId_fkey" FOREIGN KEY ("sourceId") REFERENCES "sources"("id") ON DELETE CASCADE ON UPDATE CASCADE;

-- AddForeignKey
ALTER TABLE "events" ADD CONSTRAINT "events_runId_fkey" FOREIGN KEY ("runId") REFERENCES "runs"("id") ON DELETE CASCADE ON UPDATE CASCADE;

-- AddForeignKey
ALTER TABLE "events" ADD CONSTRAINT "events_propertyKey_fkey" FOREIGN KEY ("propertyKey") REFERENCES "properties"("key") ON DELETE CASCADE ON UPDATE CASCADE;

-- AddForeignKey
ALTER TABLE "decision_trees" ADD CONSTRAINT "decision_trees_tenantId_fkey" FOREIGN KEY ("tenantId") REFERENCES "tenants"("id") ON DELETE CASCADE ON UPDATE CASCADE;

-- AddForeignKey
ALTER TABLE "evaluations" ADD CONSTRAINT "evaluations_tenantId_fkey" FOREIGN KEY ("tenantId") REFERENCES "tenants"("id") ON DELETE CASCADE ON UPDATE CASCADE;

-- AddForeignKey
ALTER TABLE "evaluations" ADD CONSTRAINT "evaluations_propertyKey_fkey" FOREIGN KEY ("propertyKey") REFERENCES "properties"("key") ON DELETE CASCADE ON UPDATE CASCADE;

-- AddForeignKey
ALTER TABLE "evaluations" ADD CONSTRAINT "evaluations_decisionTreeId_fkey" FOREIGN KEY ("decisionTreeId") REFERENCES "decision_trees"("id") ON DELETE SET NULL ON UPDATE CASCADE;

-- AddForeignKey
ALTER TABLE "outreach" ADD CONSTRAINT "outreach_tenantId_fkey" FOREIGN KEY ("tenantId") REFERENCES "tenants"("id") ON DELETE CASCADE ON UPDATE CASCADE;

-- AddForeignKey
ALTER TABLE "outreach" ADD CONSTRAINT "outreach_propertyKey_fkey" FOREIGN KEY ("propertyKey") REFERENCES "properties"("key") ON DELETE CASCADE ON UPDATE CASCADE;

-- AddForeignKey
ALTER TABLE "messages" ADD CONSTRAINT "messages_tenantId_fkey" FOREIGN KEY ("tenantId") REFERENCES "tenants"("id") ON DELETE CASCADE ON UPDATE CASCADE;
