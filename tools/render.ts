// Headless renderer for JS-heavy county portals: prints the rendered DOM to
// stdout so the C++ engine can consume it like any other fetched body.
//   DD_RENDERER="npx tsx tools/render.ts" ./build/datadiver
//   diver> render+https://some-react-county-portal.gov/parcels
import puppeteer from "puppeteer";

const url = process.argv[2];
if (!url || !/^https?:\/\//.test(url)) {
  console.error("usage: render.ts <http(s) url>");
  process.exit(2);
}

const browser = await puppeteer.launch({ headless: true });
try {
  const page = await browser.newPage();
  await page.setUserAgent("DataDiver/0.1 (public-record research)");
  await page.goto(url, { waitUntil: "networkidle2", timeout: 45_000 });
  process.stdout.write(await page.content());
} finally {
  await browser.close();
}
