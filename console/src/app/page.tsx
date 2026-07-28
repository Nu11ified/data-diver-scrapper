import { PageHeader } from "@/components/shared/page-header";
import { Card, CardContent } from "@/components/ui/card";

/**
 * Temporary placeholder for the counties dashboard. The real page (county
 * cards, add county+source form, ingest-all, live engine strip) is built by
 * the page work that follows this scaffold.
 */
export default function Home() {
  return (
    <div>
      <PageHeader
        title="Counties"
        description="County-by-county ingestion status. This dashboard is under construction; the engine status pill above is live."
      />
      <Card>
        <CardContent className="py-10 text-center text-sm text-muted-foreground">
          No pages here yet — the counties dashboard, sources workbench,
          properties, model and performance views land next.
        </CardContent>
      </Card>
    </div>
  );
}
