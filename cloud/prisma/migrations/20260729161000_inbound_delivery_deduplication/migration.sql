CREATE TABLE "inbound_deliveries" (
    "messageHandle" TEXT NOT NULL,
    "tenantId" UUID NOT NULL,
    "receivedAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,

    CONSTRAINT "inbound_deliveries_pkey" PRIMARY KEY ("messageHandle")
);

CREATE INDEX "inbound_deliveries_tenantId_receivedAt_idx"
ON "inbound_deliveries"("tenantId", "receivedAt");

ALTER TABLE "inbound_deliveries"
ADD CONSTRAINT "inbound_deliveries_tenantId_fkey"
FOREIGN KEY ("tenantId") REFERENCES "tenants"("id")
ON DELETE CASCADE ON UPDATE CASCADE;
