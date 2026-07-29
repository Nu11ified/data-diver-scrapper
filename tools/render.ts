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
  await page.goto(url, { waitUntil: "domcontentloaded", timeout: 45_000 });
  await page
    .waitForNetworkIdle({ idleTime: 1_000, timeout: 15_000 })
    .catch(() => {});
  await new Promise((resolve) => setTimeout(resolve, 2_000));
  const html = (await page.evaluate(`(() => {
    const serialize = (node) => {
      if (node.nodeType === Node.TEXT_NODE) return node.textContent ?? "";
      if (node.nodeType !== Node.ELEMENT_NODE) return "";
      const el = node;
      let attrs = "";
      for (const a of el.attributes) {
        attrs += " " + a.name + '="' + a.value.replace(/"/g, "&quot;") + '"';
      }
      let inner = "";
      if (el.shadowRoot) {
        for (const child of el.shadowRoot.childNodes) inner += serialize(child);
      }
      for (const child of el.childNodes) inner += serialize(child);
      const tag = el.tagName.toLowerCase();
      return "<" + tag + attrs + ">" + inner + "</" + tag + ">";
    };
    return "<!doctype html>" + serialize(document.documentElement);
  })()`)) as string;
  process.stdout.write(html);
} finally {
  await browser.close();
}
