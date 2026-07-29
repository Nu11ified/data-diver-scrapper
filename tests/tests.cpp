
#include <random>
#include "dd/ml/classify.hpp"
#include "dd/ml/columns.hpp"
#include "dd/core/core.hpp"
#include "dd/parse/csv.hpp"
#include "dd/parse/document.hpp"
#include "dd/parse/query.hpp"
#include "dd/engine/entity.hpp"
#include "dd/engine/events.hpp"
#include "dd/ml/features.hpp"
#include "dd/net/crawl.hpp"
#include "dd/net/fetch.hpp"
#include "dd/parse/html.hpp"
#include "dd/core/json.hpp"
#include "dd/engine/heal.hpp"
#include "dd/core/metrics.hpp"
#include "dd/ml/model.hpp"
#include "dd/parse/pdf.hpp"
#include "dd/engine/pipeline.hpp"
#include "dd/engine/bench.hpp"
#include "dd/engine/compile.hpp"
#include "dd/engine/harvest.hpp"
#include "dd/engine/schema.hpp"
#include "dd/engine/store.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <filesystem>

#if defined(DD_HAVE_ZLIB)
#include <zlib.h>
#endif

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace {
struct TestCase {
    std::string name;
    std::function<void()> body;
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct Register {
    Register(std::string name, std::function<void()> body) {
        registry().push_back(TestCase{std::move(name), std::move(body)});
    }
};

int g_failures = 0;
const char* g_current = "";

void report(const char* file, int line, const std::string& message) {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s (%s:%d): %s\n", g_current, file, line, message.c_str());
}

#define TEST(name)                                                                     \
    void test_##name();                                                                \
    const ::Register reg_##name{#name, test_##name};                                   \
    void test_##name()

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) report(__FILE__, __LINE__, "CHECK failed: " #cond);               \
    } while (0)

#define CHECK_EQ(a, b)                                                                 \
    do {                                                                               \
        const auto va = (a);                                                           \
        const auto vb = (b);                                                           \
        if (!(va == vb)) {                                                             \
            report(__FILE__, __LINE__, std::string{"CHECK_EQ failed: " #a " == " #b}); \
        }                                                                              \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                          \
    do {                                                                               \
        const double va = (a);                                                         \
        const double vb = (b);                                                         \
        if (std::fabs(va - vb) > (eps)) {                                              \
            report(__FILE__, __LINE__,                                                 \
                   "CHECK_NEAR failed: " #a "=" + std::to_string(va) + " vs " #b "=" + \
                       std::to_string(vb));                                            \
        }                                                                              \
    } while (0)

#define CHECK_THROWS(expr)                                                             \
    do {                                                                               \
        bool threw = false;                                                            \
        try {                                                                          \
            (void)(expr);                                                              \
        } catch (const dd::Error&) {                                                   \
            threw = true;                                                              \
        }                                                                              \
        if (!threw) report(__FILE__, __LINE__, "expected dd::Error from: " #expr);     \
    } while (0)

const dd::schema::Registry& test_registry() {
    static const dd::schema::Registry registry =
        dd::schema::Registry::load("data/schema.json");
    return registry;
}

bool tvalidate(const char* field, std::string_view value) {
    return dd::schema::validate(*test_registry().find(field), value);
}

std::string tnormalize(const char* field, std::string_view value) {
    return dd::schema::normalize(*test_registry().find(field), value);
}

TEST(str_basics) {
    CHECK_EQ(dd::str::trim("  a b  "), "a b");
    CHECK_EQ(dd::str::to_lower("AbC"), "abc");
    CHECK_EQ(dd::str::collapse_ws("a\n  b\t c"), "a b c");
    CHECK_EQ(dd::str::replace_all("a-b-c", "-", "+"), "a+b+c");
    CHECK_EQ(dd::str::join({"a", "b"}, ", "), "a, b");
    CHECK(dd::str::is_digits("0123"));
    CHECK(!dd::str::is_digits("12a"));
    CHECK(!dd::str::is_digits(""));
    CHECK_EQ(dd::str::strip_non_alnum("12-34/AB"), "1234AB");
}

TEST(str_tokenize_words) {
    const std::vector<std::string> expected = {"owner", "name"};
    CHECK(dd::str::tokenize_words("ownerName") == expected);
    CHECK(dd::str::tokenize_words("owner_name") == expected);
    CHECK(dd::str::tokenize_words("Owner  Name") == expected);
    CHECK(dd::str::tokenize_words("OWNER-NAME") == std::vector<std::string>({"owner", "name"}));
    CHECK_EQ(dd::str::slug("Amount Due ($)"), "amount_due");
}

TEST(str_jaro_winkler) {
    CHECK_NEAR(dd::str::jaro_winkler("parcel", "parcel"), 1.0, 1e-9);
    CHECK(dd::str::jaro_winkler("parcel", "parcell") > 0.9);
    CHECK(dd::str::jaro_winkler("owner", "taxpayer") < 0.7);
    CHECK_NEAR(dd::str::jaro_winkler("", ""), 1.0, 1e-9);
    CHECK_NEAR(dd::str::jaro_winkler("a", ""), 0.0, 1e-9);
}

TEST(str_hash_stable) {
    CHECK_EQ(dd::str::hash64("abc"), dd::str::hash64("abc"));
    CHECK(dd::str::hash64("abc") != dd::str::hash64("abd"));
    CHECK_EQ(dd::str::hex64(0xff).size(), std::size_t{16});
}

TEST(time_roundtrip) {
    const std::int64_t t = 1750000000;
    CHECK_EQ(dd::timeutil::iso_from_unix(t), "2025-06-15T15:06:40Z");
}

TEST(fileio_roundtrip) {
    const std::string dir = "build/test_tmp";
    dd::fileio::ensure_dir(dir);
    const std::string path = dir + "/roundtrip.txt";
    dd::fileio::write_file_atomic(path, "hello\nworld\n");
    CHECK(dd::fileio::exists(path));
    CHECK_EQ(dd::fileio::read_file(path), "hello\nworld\n");
    dd::fileio::append_line(path, "third");
    const std::vector<std::string> lines = dd::fileio::read_lines(path);
    CHECK_EQ(lines.size(), std::size_t{3});
    CHECK_EQ(lines[2], "third");
}

TEST(json_parse_scalars) {
    CHECK(dd::json::parse("null").is_null());
    CHECK_EQ(dd::json::parse("true").as_bool(), true);
    CHECK_NEAR(dd::json::parse("-12.5e1").as_number(), -125.0, 1e-9);
    CHECK_EQ(dd::json::parse("\"a\\nb\"").as_string(), "a\nb");
    CHECK_EQ(dd::json::parse("\"\\u0041\\u00e9\"").as_string(), "A\xC3\xA9");
    CHECK_EQ(dd::json::parse("\"\\ud83d\\ude00\"").as_string(), "\xF0\x9F\x98\x80");
}

TEST(json_parse_structures) {
    const dd::json::Value v = dd::json::parse(R"({"a": [1, 2, {"b": "c"}], "d": null})");
    CHECK(v.is_object());
    CHECK_EQ(v.members().size(), std::size_t{2});
    const dd::json::Value* a = v.find("a");
    CHECK(a != nullptr && a->is_array());
    CHECK_EQ(a->items().size(), std::size_t{3});
    CHECK_EQ(a->items()[2].find("b")->as_string(), "c");
    CHECK(v.find("missing") == nullptr);
}

TEST(json_member_order_preserved) {
    const dd::json::Value v = dd::json::parse(R"({"z": 1, "a": 2, "m": 3})");
    CHECK_EQ(v.members()[0].first, "z");
    CHECK_EQ(v.members()[1].first, "a");
    CHECK_EQ(v.members()[2].first, "m");
}

TEST(json_rejects_malformed) {
    CHECK_THROWS(dd::json::parse("{"));
    CHECK_THROWS(dd::json::parse("[1,]"));
    CHECK_THROWS(dd::json::parse("{\"a\" 1}"));
    CHECK_THROWS(dd::json::parse("tru"));
    CHECK_THROWS(dd::json::parse("1 2"));
    CHECK_THROWS(dd::json::parse("\"unterminated"));
}

TEST(json_serialize_roundtrip) {
    const std::string text = R"({"a":[1,2.5,"x"],"b":{"c":true,"d":null}})";
    CHECK_EQ(dd::json::parse(text).serialize(), text);
}

TEST(json_writer) {
    dd::json::Writer w;
    w.begin_object();
    w.field("name", "a\"b");
    w.field("n", 3);
    w.key("list");
    w.begin_array();
    w.number_value(1.5);
    w.bool_value(false);
    w.null_value();
    w.end_array();
    w.end_object();
    CHECK_EQ(w.str(), R"({"name":"a\"b","n":3,"list":[1.5,false,null]})");
}

TEST(html_basic_tree) {
    const dd::html::Document doc = dd::html::parse(
        "<html><head><title>Tax Roll</title></head>"
        "<body><h1>Delinquent List</h1><p>Hello <b>world</b></p></body></html>");
    CHECK_EQ(doc.title(), "Tax Roll");
    const std::vector<std::string> headings = doc.headings();
    CHECK_EQ(headings.size(), std::size_t{1});
    CHECK_EQ(headings[0], "Delinquent List");
    CHECK_EQ(doc.find_all("b").size(), std::size_t{1});
    CHECK(dd::str::contains(doc.text(), "Hello world"));
}

TEST(crawl_resolves_urls_against_the_page) {
    const std::string base = "https://county.gov/tax/sales/june.html?year=2026";
    CHECK_EQ(dd::crawl::resolve_url(base, "list.html"),
             std::string{"https://county.gov/tax/sales/list.html"});
    CHECK_EQ(dd::crawl::resolve_url(base, "/parcels/1"),
             std::string{"https://county.gov/parcels/1"});
    CHECK_EQ(dd::crawl::resolve_url(base, "../roll.csv"),
             std::string{"https://county.gov/tax/roll.csv"});
    CHECK_EQ(dd::crawl::resolve_url(base, "//cdn.county.gov/a.json"),
             std::string{"https://cdn.county.gov/a.json"});
    CHECK_EQ(dd::crawl::resolve_url(base, "https://other.gov/x"),
             std::string{"https://other.gov/x"});
    CHECK_EQ(dd::crawl::resolve_url(base, "page.html#row3"),
             std::string{"https://county.gov/tax/sales/page.html"});
    // Links that go nowhere a crawler can follow
    CHECK(dd::crawl::resolve_url(base, "#top").empty());
    CHECK(dd::crawl::resolve_url(base, "mailto:clerk@county.gov").empty());
    CHECK(dd::crawl::resolve_url(base, "javascript:void(0)").empty());
    CHECK_EQ(dd::crawl::host_of("https://County.GOV/x"), std::string{"county.gov"});
}

TEST(crawl_resolves_query_only_links_onto_the_same_page) {
    // Query-driven pagination is the commonest shape on county result pages,
    // and dropping the path walks the crawler off the results entirely.
    const std::string base = "https://county.gov/tax/list";
    CHECK_EQ(dd::crawl::resolve_url(base, "?page=2"),
             std::string{"https://county.gov/tax/list?page=2"});
    CHECK_EQ(dd::crawl::resolve_url("https://county.gov/tax/list?page=1", "?page=2"),
             std::string{"https://county.gov/tax/list?page=2"});
    CHECK_EQ(dd::crawl::resolve_url("https://county.gov/tax/", "?page=3"),
             std::string{"https://county.gov/tax/?page=3"});
    // A url whose host is followed straight by a query still parses as a host.
    CHECK_EQ(dd::crawl::host_of("https://county.gov?x=1"), std::string{"county.gov"});
    CHECK_EQ(dd::crawl::resolve_url("https://county.gov?x=1", "?page=2"),
             std::string{"https://county.gov/?page=2"});
}

TEST(crawl_obeys_robots_txt) {
    const std::string text =
        "User-agent: *\n"
        "Disallow: /private\n"
        "Disallow: /search\n"
        "Allow: /search/results\n"
        "Crawl-delay: 2\n"
        "\n"
        "User-agent: EvilBot\n"
        "Disallow: /\n";
    const dd::crawl::Robots robots =
        dd::crawl::Robots::parse(text, "DataDiver/0.1 (public-record research)");
    CHECK(robots.allowed("/tax/sales"));
    CHECK(!robots.allowed("/private/notes"));
    CHECK(!robots.allowed("/search"));
    CHECK(robots.allowed("/search/results/page2")); // longer Allow wins
    CHECK_EQ(robots.crawl_delay_seconds(), 2.0);

    // A record naming our agent overrides the wildcard record.
    const std::string mine =
        "User-agent: *\nDisallow: /\n\nUser-agent: datadiver\nDisallow: /admin\n";
    const dd::crawl::Robots ours = dd::crawl::Robots::parse(mine, "DataDiver/0.1");
    CHECK(ours.allowed("/tax"));
    CHECK(!ours.allowed("/admin/panel"));

    CHECK(dd::crawl::Robots::allow_all().allowed("/anything"));
}

TEST(crawl_prefers_pagination_links) {
    const std::string page =
        "<html><body>"
        "<a href='/help'>Help</a>"
        "<table><tr><td><a href='/parcel/1'>101-22</a></td></tr></table>"
        "<a href='/list?page=2' rel='next'>Next</a>"
        "<a href='/list?page=3'>3</a>"
        "</body></html>";
    const std::vector<std::string> links =
        dd::crawl::links_from("https://county.gov/list", page, 10);
    CHECK_EQ(links.size(), std::size_t{4});
    // Pagination is queued first so a multi-page table is walked before depth.
    CHECK_EQ(links[0], std::string{"https://county.gov/list?page=2"});
    CHECK_EQ(links[1], std::string{"https://county.gov/list?page=3"});
    CHECK_EQ(dd::crawl::links_from("https://county.gov/list", page, 1).size(), std::size_t{1});
}

TEST(css_selector_matches_like_a_dom) {
    const dd::html::Document doc = dd::html::parse(
        "<html><body>"
        "<table class='results Sortable' id='main'>"
        "  <thead><tr><th>Parcel</th></tr></thead>"
        "  <tbody>"
        "    <tr class='row odd'><td class='parcel'>101-22</td><td>SMITH, JANE</td></tr>"
        "    <tr class='row even'><td class='parcel'>101-23</td><td>ACME LLC</td></tr>"
        "  </tbody>"
        "</table>"
        "<table class='nav'><tbody><tr><td class='parcel'>ignore me</td></tr></tbody></table>"
        "<a href='/next' data-page='2'>Next</a>"
        "</body></html>");

    CHECK_EQ(dd::html::query_all(doc, "tr").size(), std::size_t{4});
    CHECK_EQ(dd::html::query_all(doc, "table.results tr").size(), std::size_t{3});
    CHECK_EQ(dd::html::query_all(doc, "table.results > tbody > tr").size(), std::size_t{2});
    CHECK_EQ(dd::html::query_all(doc, "#main tbody td.parcel").size(), std::size_t{2});
    CHECK_EQ(dd::html::query_all(doc, "table.results.sortable tbody tr").size(), std::size_t{2});
    CHECK_EQ(dd::html::query_all(doc, "th, a").size(), std::size_t{2});
    CHECK_EQ(dd::html::query_all(doc, "[data-page]").size(), std::size_t{1});
    CHECK_EQ(dd::html::query_all(doc, "[data-page='2']").size(), std::size_t{1});
    CHECK_EQ(dd::html::query_all(doc, "[data-page='9']").size(), std::size_t{0});
    CHECK_EQ(dd::html::query_all(doc, "tbody > td").size(), std::size_t{0}); // td is not a tbody child

    const dd::html::Node* first = dd::html::query(doc, "table.results tbody td.parcel");
    CHECK(first != nullptr);
    CHECK_EQ(first->text_content(), "101-22");

    // The nav table's cell must not be reachable through the results table.
    const std::vector<const dd::html::Node*> parcels =
        dd::html::query_all(doc, "table.results td.parcel");
    CHECK_EQ(parcels.size(), std::size_t{2});
    CHECK_EQ(parcels[1]->text_content(), "101-23");

    CHECK_THROWS(dd::html::css("table..bad"));
    CHECK_THROWS(dd::html::css(""));
    // A dangling combinator silently became "table", which quietly returns the
    // whole table when the caller asked for a child.
    CHECK_THROWS(dd::html::css("table >"));
    CHECK_THROWS(dd::html::css("table > "));
}

TEST(dom_traversal_axes) {
    const dd::html::Document doc = dd::html::parse(
        "<div class='card'><span>a</span><span>b</span>text<span>c</span></div>");
    const dd::html::Node* card = dd::html::query(doc, "div.card");
    CHECK(card != nullptr);
    const std::vector<const dd::html::Node*> kids = dd::html::element_children(card);
    CHECK_EQ(kids.size(), std::size_t{3}); // the bare text node is not an element
    CHECK_EQ(dd::html::next_element(kids[0])->text_content(), "b");
    CHECK_EQ(dd::html::previous_element(kids[2])->text_content(), "b");
    CHECK(dd::html::next_element(kids[2]) == nullptr);
    CHECK(dd::html::previous_element(kids[0]) == nullptr);
    CHECK_EQ(dd::html::closest(kids[1], dd::html::css("div.card")), card);
    CHECK(dd::html::closest(kids[1], dd::html::css("table")) == nullptr);
}

TEST(html_recovers_from_unclosed_tags) {
    const dd::html::Document doc = dd::html::parse(
        "<table><tr><td>a<td>b<tr><td>c<td>d</table>");
    const std::vector<const dd::html::Node*> rows = doc.find_all("tr");
    CHECK_EQ(rows.size(), std::size_t{2});
    CHECK_EQ(rows[0]->element_child_count(), std::size_t{2});
    CHECK_EQ(rows[1]->element_child_count(), std::size_t{2});
    CHECK_EQ(rows[1]->children[0]->text_content(), "c");
}

TEST(html_drops_stray_close_tags) {
    const dd::html::Document doc = dd::html::parse("<div>a</span></div><p>b</p>");
    CHECK_EQ(doc.find_all("div").size(), std::size_t{1});
    CHECK_EQ(doc.find_all("p").size(), std::size_t{1});
    CHECK(dd::str::contains(doc.text(), "a b"));
}

TEST(html_attributes_and_entities) {
    const dd::html::Document doc = dd::html::parse(
        "<td class=\"Owner-Name main\" data-field=taxpayer>Smith &amp; Sons&#44; LLC</td>");
    const std::vector<const dd::html::Node*> cells = doc.find_all("td");
    CHECK_EQ(cells.size(), std::size_t{1});
    const dd::html::Node* cell = cells[0];
    CHECK(cell->attr("data-field") != nullptr);
    CHECK_EQ(*cell->attr("data-field"), "taxpayer");
    const std::vector<std::string> classes = cell->classes();
    CHECK_EQ(classes.size(), std::size_t{2});
    CHECK_EQ(classes[0], "owner-name");
    CHECK_EQ(cell->text_content(), "Smith & Sons, LLC");
    CHECK_EQ(cell->signature(), "td.main.owner-name");
}

TEST(html_script_content_is_raw) {
    const dd::html::Document doc = dd::html::parse(
        "<script>if (a < b) { render(\"<td>\"); }</script><p>visible</p>");
    CHECK_EQ(doc.find_all("td").size(), std::size_t{0});
    CHECK_EQ(doc.find_all("p").size(), std::size_t{1});
}

TEST(html_own_text_vs_text_content) {
    const dd::html::Document doc =
        dd::html::parse("<div><span>Owner:</span> Jane Smith</div>");
    const dd::html::Node* div = doc.find_all("div")[0];
    CHECK_EQ(div->own_text(), "Jane Smith");
    CHECK_EQ(div->text_content(), "Owner: Jane Smith");
}

TEST(html_void_and_comments) {
    const dd::html::Document doc = dd::html::parse(
        "<!-- hidden <td>x</td> --><br><img src=\"a.png\"><p>after</p>");
    CHECK_EQ(doc.find_all("td").size(), std::size_t{0});
    CHECK_EQ(doc.find_all("p").size(), std::size_t{1});
    CHECK_EQ(doc.find_all("img").size(), std::size_t{1});
}

TEST(csv_basic) {
    const dd::csv::Table t = dd::csv::parse("Parcel,Owner,Amount\n123,Smith,10.50\n456,Jones,7\n");
    CHECK_EQ(t.header.size(), std::size_t{3});
    CHECK_EQ(t.header[1], "Owner");
    CHECK_EQ(t.rows.size(), std::size_t{2});
    CHECK_EQ(t.rows[1][1], "Jones");
}

TEST(csv_quotes_and_embedded_delims) {
    const dd::csv::Table t = dd::csv::parse(
        "Name,Address\n\"Smith, Jane\",\"1402 Main St\nUnit 2\"\n\"He said \"\"hi\"\"\",x\n");
    CHECK_EQ(t.rows.size(), std::size_t{2});
    CHECK_EQ(t.rows[0][0], "Smith, Jane");
    CHECK_EQ(t.rows[0][1], "1402 Main St\nUnit 2");
    CHECK_EQ(t.rows[1][0], "He said \"hi\"");
}

TEST(csv_delimiter_detection) {
    CHECK_EQ(dd::csv::detect_delimiter("a;b;c\n1;2;3"), ';');
    CHECK_EQ(dd::csv::detect_delimiter("a\tb\tc"), '\t');
    CHECK_EQ(dd::csv::detect_delimiter("a|b|c"), '|');
    const dd::csv::Table t = dd::csv::parse("Parcel;Owner\n1;Smith\n");
    CHECK_EQ(t.header.size(), std::size_t{2});
    CHECK_EQ(t.rows[0][1], "Smith");
}

TEST(csv_headerless_numeric_first_row) {
    const dd::csv::Table t = dd::csv::parse("123,Smith\n456,Jones\n");
    CHECK(t.header.empty());
    CHECK_EQ(t.rows.size(), std::size_t{2});
}

TEST(metrics_report_real_values) {
    CHECK(dd::metrics::current_rss_bytes() > 1024 * 1024);
    CHECK(dd::metrics::peak_rss_bytes() >= dd::metrics::current_rss_bytes() / 2);
    CHECK(dd::metrics::cpu_time_ms() > 0.0);
}

TEST(fetch_local_file) {
    const std::string dir = "build/test_tmp";
    dd::fileio::ensure_dir(dir);
    const std::string path = dir + "/fetch_me.txt";
    dd::fileio::write_file_atomic(path, "payload bytes");
    const dd::fetch::Result r = dd::fetch::get(path);
    CHECK(r.ok);
    CHECK_EQ(r.body, "payload bytes");
    CHECK_EQ(r.bytes, std::int64_t{13});
    CHECK(r.total_ms >= 0.0);
    CHECK(!r.fetched_at.empty());

    const dd::fetch::Result via_scheme = dd::fetch::get("file://" + path);
    CHECK(via_scheme.ok);
    CHECK_EQ(via_scheme.body, "payload bytes");
}

TEST(fetch_missing_file_reports_error) {
    const dd::fetch::Result r = dd::fetch::get("build/test_tmp/does_not_exist.txt");
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    CHECK_EQ(r.bytes, std::int64_t{0});
}

TEST(fetch_rejects_unknown_scheme) {
    const dd::fetch::Result r = dd::fetch::get("gopher://example.org/1");
    CHECK(!r.ok);
    CHECK(dd::str::contains(r.error, "scheme"));
}

namespace pdfgen {
std::string make_pdf(const std::string& content, bool compress) {
    std::string stream_data = content;
    std::string filter;
#if defined(DD_HAVE_ZLIB)
    if (compress) {
        uLongf bound = compressBound(static_cast<uLong>(content.size()));
        std::string packed(bound, '\0');
        if (compress2(reinterpret_cast<Bytef*>(packed.data()), &bound,
                      reinterpret_cast<const Bytef*>(content.data()),
                      static_cast<uLong>(content.size()), 6) == Z_OK) {
            packed.resize(bound);
            stream_data = packed;
            filter = " /Filter /FlateDecode";
        }
    }
#else
    (void)compress;
#endif
    std::string pdf = "%PDF-1.4\n";
    pdf += "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n";
    pdf += "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n";
    pdf += "3 0 obj << /Type /Page /Parent 2 0 R /Contents 4 0 R >> endobj\n";
    pdf += "4 0 obj << /Length " + std::to_string(stream_data.size()) + filter +
           " >>\nstream\n";
    pdf += stream_data;
    pdf += "\nendstream\nendobj\n";
    pdf += "%%EOF\n";
    return pdf;
}
} // namespace pdfgen

TEST(pdf_rejects_non_pdf) { CHECK_THROWS(dd::pdf::extract_text_lines("<html></html>")); }

TEST(pdf_plain_stream_text) {
    const std::string content =
        "BT /F1 10 Tf 72 720 Td (Delinquent Tax List) Tj 0 -14 Td (Parcel  Owner) Tj "
        "0 -14 Td [(123-456) -500 (Jane Smith)] TJ ET";
    const std::string pdf = pdfgen::make_pdf(content, false);
    const std::vector<std::string> lines = dd::pdf::extract_text_lines(pdf);
    CHECK_EQ(lines.size(), std::size_t{3});
    CHECK_EQ(lines[0], "Delinquent Tax List");
    CHECK(dd::str::contains(lines[2], "123-456"));
    CHECK(dd::str::contains(lines[2], "Jane Smith"));
    CHECK(!dd::str::contains(lines[2], "500"));
}

#if defined(DD_HAVE_ZLIB)
TEST(pdf_flate_stream_text) {
    const std::string content = "BT (Compressed Roll) Tj 0 -12 Td (Parcel 42) Tj ET";
    const std::string pdf = pdfgen::make_pdf(content, true);
    CHECK(dd::str::contains(pdf, "FlateDecode"));
    const std::vector<std::string> lines = dd::pdf::extract_text_lines(pdf);
    CHECK_EQ(lines.size(), std::size_t{2});
    CHECK_EQ(lines[0], "Compressed Roll");
    CHECK_EQ(lines[1], "Parcel 42");
}
#endif

TEST(pdf_string_escapes) {
    const std::string content = "BT (Smith \\(Jane\\) \\\\ Co \\052) Tj ET";
    const std::string pdf = pdfgen::make_pdf(content, false);
    const std::vector<std::string> lines = dd::pdf::extract_text_lines(pdf);
    CHECK_EQ(lines.size(), std::size_t{1});
    CHECK_EQ(lines[0], "Smith (Jane) \\ Co *");
}

TEST(document_detects_formats) {
    using dd::doc::Format;
    CHECK(dd::doc::detect_format("", "%PDF-1.4 xxxxxxxx") == Format::Pdf);
    CHECK(dd::doc::detect_format("", "{\"a\": 1}") == Format::Json);
    CHECK(dd::doc::detect_format("", "[1, 2]") == Format::Json);
    CHECK(dd::doc::detect_format("", "<!DOCTYPE html><html></html>") == Format::Html);
    CHECK(dd::doc::detect_format("", "<div>x</div>") == Format::Html);
    CHECK(dd::doc::detect_format("", "a,b,c\n1,2,3\n4,5,6\n") == Format::Csv);
    CHECK(dd::doc::detect_format("text/csv", "weird single line") == Format::Csv);
    CHECK(dd::doc::detect_format("", "just words here") == Format::Text);
    CHECK(dd::doc::detect_format("", "{broken") == Format::Text);
}

TEST(document_html_table_records) {
    const std::string page =
        "<html><head><title>Delinquency</title></head><body><h2>Tax Roll</h2>"
        "<table class=\"roll\">"
        "<tr><th>Parcel Number</th><th>Owner Name</th><th>Amount Due</th></tr>"
        "<tr><td>123-456-789</td><td>Jane Smith</td><td>$8,421.37</td></tr>"
        "<tr><td>987-654-321</td><td>Bob Ray</td><td>$120.00</td></tr>"
        "</table></body></html>";
    const dd::doc::Model m = dd::doc::build_auto("text/html", page);
    CHECK(m.format == dd::doc::Format::Html);
    CHECK_EQ(m.title, "Delinquency");
    CHECK_EQ(m.records.size(), std::size_t{2});
    CHECK_EQ(m.labels.size(), std::size_t{3});
    const dd::doc::Cell* owner = m.records[0].find("Owner Name");
    CHECK(owner != nullptr);
    CHECK_EQ(owner->value, "Jane Smith");
    CHECK(!m.records[0].cells[0].path.empty());
    CHECK(dd::str::contains(m.container_signature, "table"));
}

TEST(document_html_repeated_blocks) {
    std::string page = "<html><body><div id=\"list\">";
    const char* owners[] = {"Jane Smith", "Bob Ray", "Ann Lee"};
    const char* parcels[] = {"111-222", "333-444", "555-666"};
    for (int i = 0; i < 3; ++i) {
        page += std::string{"<div class=\"case\">"} +
                "<span class=\"owner-name\">" + owners[i] + "</span>" +
                "<span class=\"parcel-id\">" + parcels[i] + "</span>" +
                "<div><b>Status:</b> Delinquent</div>" +
                "</div>";
    }
    page += "</div></body></html>";
    const dd::doc::Model m = dd::doc::build_auto("text/html", page);
    CHECK_EQ(m.records.size(), std::size_t{3});
    const dd::doc::Cell* owner = m.records[1].find("owner-name");
    CHECK(owner != nullptr);
    CHECK_EQ(owner->value, "Bob Ray");
    const dd::doc::Cell* status = m.records[0].find("Status");
    CHECK(status != nullptr);
    CHECK_EQ(status->value, "Delinquent");
}

TEST(document_html_data_attributes) {
    std::string page = "<body>";
    for (int i = 0; i < 3; ++i) {
        page += "<div class=\"r\"><span data-field=\"taxpayer\">P" + std::to_string(i) +
                "</span><span data-field=\"account\">A" + std::to_string(i) + "</span></div>";
    }
    page += "</body>";
    const dd::doc::Model m = dd::doc::build_auto("text/html", page);
    CHECK_EQ(m.records.size(), std::size_t{3});
    const dd::doc::Cell* taxpayer = m.records[2].find("taxpayer");
    CHECK(taxpayer != nullptr);
    CHECK_EQ(taxpayer->value, "P2");
}

TEST(document_json_records) {
    const std::string body = R"({
      "meta": {"count": 2},
      "results": [
        {"apn": "123", "owner": {"name": "Jane"}, "tags": ["a", "b"], "due": 42.5},
        {"apn": "456", "owner": {"name": "Bob"}, "tags": [], "due": 7}
      ]
    })";
    const dd::doc::Model m = dd::doc::build_auto("application/json", body);
    CHECK(m.format == dd::doc::Format::Json);
    CHECK_EQ(m.records.size(), std::size_t{2});
    const dd::doc::Cell* name = m.records[0].find("owner.name");
    CHECK(name != nullptr);
    CHECK_EQ(name->value, "Jane");
    const dd::doc::Cell* tags = m.records[0].find("tags");
    CHECK(tags != nullptr);
    CHECK_EQ(tags->value, "a; b");
    const dd::doc::Cell* due = m.records[1].find("due");
    CHECK(due != nullptr);
    CHECK_EQ(due->value, "7");
    CHECK(dd::str::contains(m.container_signature, "results"));
}

TEST(document_json_single_object) {
    const dd::doc::Model m =
        dd::doc::build_auto("application/json", R"({"parcel": "1", "owner": "X"})");
    CHECK_EQ(m.records.size(), std::size_t{1});
    CHECK_EQ(m.records[0].cells.size(), std::size_t{2});
}

TEST(document_csv_records) {
    const dd::doc::Model m = dd::doc::build_auto(
        "text/csv", "Account,Taxpayer,Balance\nA1,Jane,100.5\nA2,Bob,7\n");
    CHECK(m.format == dd::doc::Format::Csv);
    CHECK_EQ(m.records.size(), std::size_t{2});
    const dd::doc::Cell* who = m.records[1].find("Taxpayer");
    CHECK(who != nullptr);
    CHECK_EQ(who->value, "Bob");
}

TEST(document_text_columns) {
    const std::string body =
        "County Tax Report\n"
        "Parcel      Owner        Due\n"
        "111-22      Jane Smith   500.00\n"
        "333-44      Bob Ray      75.10\n"
        "555-66      Ann Lee      20.00\n";
    const dd::doc::Model m = dd::doc::build_auto("text/plain", body);
    CHECK(m.format == dd::doc::Format::Text);
    CHECK_EQ(m.records.size(), std::size_t{3});
    const dd::doc::Cell* owner = m.records[0].find("Owner");
    CHECK(owner != nullptr);
    CHECK_EQ(owner->value, "Jane Smith");
}

TEST(document_fingerprint_tracks_shape) {
    const std::string a = "Parcel,Owner\n1,Jane\n";
    const std::string b = "Parcel,Owner\n2,Bob\n3,Sue\n";
    const std::string c = "APN,Taxpayer\n1,Jane\n";
    const dd::doc::Model ma = dd::doc::build_auto("text/csv", a);
    const dd::doc::Model mb = dd::doc::build_auto("text/csv", b);
    const dd::doc::Model mc = dd::doc::build_auto("text/csv", c);
    CHECK_EQ(ma.structure_fingerprint(), mb.structure_fingerprint());
    CHECK(ma.structure_fingerprint() != mc.structure_fingerprint());
}

TEST(document_empty_yields_no_records) {
    const dd::doc::Model m = dd::doc::build_auto("text/html", "<html><body></body></html>");
    CHECK(m.records.empty());
}

TEST(features_prefix_and_filter) {
    const std::string page =
        "<html><head><title>Delinquent Roll</title></head><body><h1>The County List</h1>"
        "<table><tr><th>Owner Name</th><th>Amount</th></tr>"
        "<tr><td>Jane</td><td>42</td></tr><tr><td>Bob</td><td>7</td></tr></table></body></html>";
    const dd::doc::Model m = dd::doc::build_auto("text/html", page);
    const dd::features::Bag bag = dd::features::extract(m, "https://county.example/tax/delinquent-list");
    CHECK(bag.count("fmt:html") == 1);
    CHECK(bag.count("label:owner") == 1);
    CHECK(bag.count("title:delinquent") == 1);
    CHECK(bag.count("h:county") == 1);
    CHECK(bag.count("url:delinquent") == 1);
    CHECK(bag.count("the") == 0);       // stopword
    CHECK(bag.count("url:https") == 0); // stopword
}

TEST(model_learns_and_reports_posteriors) {
    dd::model::NaiveBayes nb;
    dd::features::Bag apples{{"apple", 3}, {"orchard", 1}};
    dd::features::Bag apples2{{"apple", 2}, {"cider", 1}};
    dd::features::Bag boats{{"boat", 3}, {"harbor", 1}};
    dd::features::Bag boats2{{"boat", 1}, {"sail", 2}};
    nb.add_example("fruit", apples);
    nb.add_example("fruit", apples2);
    nb.add_example("marine", boats);
    nb.add_example("marine", boats2);

    const std::vector<dd::model::Scored> scored = nb.predict({{"apple", 2}, {"cider", 1}});
    CHECK_EQ(scored.size(), std::size_t{2});
    CHECK_EQ(scored[0].label, "fruit");
    CHECK(scored[0].probability > 0.8);
    CHECK_NEAR(scored[0].probability + scored[1].probability, 1.0, 1e-9);
}

TEST(model_serialize_roundtrip) {
    dd::model::NaiveBayes nb;
    nb.add_example("a", {{"x", 2}, {"y", 1}});
    nb.add_example("b", {{"z", 3}});
    const std::string text = nb.serialize();
    const dd::model::NaiveBayes loaded = dd::model::NaiveBayes::deserialize(text);
    const dd::features::Bag probe{{"z", 1}};
    const auto before = nb.predict(probe);
    const auto after = loaded.predict(probe);
    CHECK_EQ(before[0].label, after[0].label);
    CHECK_NEAR(before[0].probability, after[0].probability, 1e-12);
}

TEST(model_alpha_shapes_posterior_and_survives_roundtrip) {
    dd::model::NaiveBayes sharp;
    sharp.set_alpha(0.1);
    dd::model::NaiveBayes smooth;
    smooth.set_alpha(5.0);
    for (dd::model::NaiveBayes* nb : {&sharp, &smooth}) {
        nb->add_example("fruit", {{"apple", 3}, {"orchard", 1}});
        nb->add_example("marine", {{"boat", 3}, {"harbor", 1}});
    }
    const dd::features::Bag probe{{"apple", 2}};
    const auto sharp_scored = sharp.predict(probe);
    const auto smooth_scored = smooth.predict(probe);
    CHECK_EQ(sharp_scored[0].label, "fruit");
    CHECK_EQ(smooth_scored[0].label, "fruit");
    CHECK(sharp_scored[0].probability > smooth_scored[0].probability);

    const dd::model::NaiveBayes loaded = dd::model::NaiveBayes::deserialize(sharp.serialize());
    CHECK_NEAR(loaded.predict(probe)[0].probability, sharp_scored[0].probability, 1e-12);
    CHECK_THROWS(sharp.set_alpha(0.0));
}

TEST(model_rejects_bad_serialization) {
    CHECK_THROWS(dd::model::NaiveBayes::deserialize("{\"kind\":\"other\"}"));
    CHECK_THROWS(dd::model::NaiveBayes::deserialize(
        "{\"kind\":\"naive_bayes_multinomial\",\"classes\":[]}"));
}

TEST(bench_scores_mapping_against_answer_key) {
    const dd::schema::Registry registry = dd::schema::Registry::from_json(R"({"fields": [
        {"name": "a", "kind": "id", "identity": true, "synonyms": ["a"]},
        {"name": "b", "kind": "text", "synonyms": ["b"]},
        {"name": "c", "kind": "text", "synonyms": ["c"]}
    ]})");
    dd::bench::Golden golden;
    golden.source_id = "s";
    golden.classifications = {"tax_delinquency", "trustee_auction"};
    golden.fields["a"] = {"col_x"};
    golden.fields["b"] = {"col_y", ""};

    CHECK(dd::bench::classification_ok(golden, "trustee_auction"));
    CHECK(!dd::bench::classification_ok(golden, "probate_case"));

    const dd::bench::MappingScore score = dd::bench::score_mapping(
        golden, registry, {{"a", "col_x"}, {"c", "col_z"}});
    CHECK_EQ(score.tp, std::size_t{1});
    CHECK_EQ(score.spurious, std::size_t{1});
    CHECK_EQ(score.missing, std::size_t{0});
    CHECK_NEAR(score.precision(), 0.5, 1e-9);
    CHECK_NEAR(score.recall(), 1.0, 1e-9);

    const dd::bench::MappingScore missing =
        dd::bench::score_mapping(golden, registry, {{"b", "col_y"}});
    CHECK_EQ(missing.missing, std::size_t{1});
    CHECK_EQ(missing.tp, std::size_t{1});

    const dd::bench::MappingScore wrong =
        dd::bench::score_mapping(golden, registry, {{"a", "col_wrong"}});
    CHECK_EQ(wrong.spurious, std::size_t{1});
    CHECK_EQ(wrong.missing, std::size_t{1});
    CHECK_NEAR(wrong.recall(), 0.0, 1e-9);
}

dd::columns::Hyper tiny_hyper() {
    dd::columns::Hyper h;
    h.seq_len = 16;
    h.d_model = 8;
    h.heads = 2;
    h.layers = 1;
    h.d_ffn = 12;
    return h;
}

std::vector<dd::columns::Example> synthetic_columns(std::size_t per_class,
                                                    std::uint32_t seed) {
    std::mt19937 rng{seed};
    std::vector<dd::columns::Example> out;
    for (std::size_t i = 0; i < per_class; ++i) {
        dd::columns::Example digits;
        digits.name = i % 2 == 0 ? "acct" : "num";
        for (int v = 0; v < 3; ++v) {
            digits.values.push_back(std::to_string(1000 + static_cast<int>(rng() % 9000)));
        }
        digits.label = "digits";
        digits.domain = "domain" + std::to_string(i % 7);
        out.push_back(digits);

        dd::columns::Example words;
        words.name = i % 2 == 0 ? "person" : "who";
        for (int v = 0; v < 3; ++v) {
            static const char* kNames[] = {"maria lopez", "james hill", "ada okafor",
                                           "chen wei", "sam ortiz"};
            words.values.push_back(kNames[rng() % 5]);
        }
        words.label = "words";
        words.domain = "domain" + std::to_string(i % 7);
        out.push_back(words);
    }
    return out;
}

TEST(columns_tokenizer_budgets_name_and_values) {
    const std::vector<int> ids = dd::columns::tokenize(
        "Owner Name", {"MARIA LOPEZ", "JAMES HILL"}, 64);
    CHECK(ids.size() <= 64);
    CHECK_EQ(ids[0], 1);  // CLS
    CHECK_EQ(dd::columns::tokenize("O", {}, 8)[1], dd::columns::tokenize("o", {}, 8)[1]);
    CHECK_EQ(dd::columns::tokenize(std::string(500, 'x'), {std::string(500, 'y')}, 32).size(),
             std::size_t{32});
}

TEST(columns_gradients_match_finite_differences) {
    const std::vector<dd::columns::Example> data = synthetic_columns(4, 11);
    dd::columns::ColumnModel model{tiny_hyper()};
    dd::columns::TrainConfig config;
    config.epochs = 0;
    config.seed = 3;
    model.train(data, {}, config);  // initialises weights and classes only

    const dd::columns::Example& probe = data.front();
    const std::vector<double> grad = model.gradient_for(probe);
    std::vector<double>& params = model.raw_parameters();
    CHECK_EQ(grad.size(), params.size());

    std::mt19937 rng{17};
    const double eps = 1e-6;
    std::size_t checked = 0;
    for (int trial = 0; trial < 60; ++trial) {
        const std::size_t i = rng() % params.size();
        const double saved = params[i];
        params[i] = saved + eps;
        const double up = model.loss_for(probe);
        params[i] = saved - eps;
        const double down = model.loss_for(probe);
        params[i] = saved;
        const double numeric = (up - down) / (2.0 * eps);
        if (std::abs(numeric) < 1e-8 && std::abs(grad[i]) < 1e-8) continue;
        const double rel = std::abs(numeric - grad[i]) /
                           std::max({std::abs(numeric), std::abs(grad[i]), 1e-8});
        CHECK(std::abs(numeric - grad[i]) < 1e-9 || rel < 1e-4);
        ++checked;
    }
    CHECK(checked > 20);
}

TEST(columns_model_learns_value_shapes) {
    std::vector<dd::columns::Example> train;
    std::vector<dd::columns::Example> holdout;
    dd::columns::split_by_domain(synthetic_columns(40, 5), 4, &train, &holdout);
    CHECK(!holdout.empty());

    dd::columns::ColumnModel model{tiny_hyper()};
    dd::columns::TrainConfig config;
    config.epochs = 12;
    config.batch = 16;
    config.threads = 2;
    const dd::columns::TrainReport train_report = model.train(train, holdout, config);
    CHECK(train_report.epoch_loss.front() > train_report.epoch_loss.back());
    CHECK(train_report.holdout_accuracy > 0.9);

    const dd::columns::Prediction digits = model.predict("zzqx", {"4821", "9034", "1187"});
    CHECK_EQ(digits.label, "digits");
    const dd::columns::Prediction words = model.predict("zzqx", {"maria lopez", "chen wei"});
    CHECK_EQ(words.label, "words");
}

TEST(columns_model_serializes_roundtrip) {
    std::vector<dd::columns::Example> data = synthetic_columns(6, 9);
    dd::columns::ColumnModel model{tiny_hyper()};
    dd::columns::TrainConfig config;
    config.epochs = 2;
    model.train(data, {}, config);

    const dd::columns::ColumnModel loaded =
        dd::columns::ColumnModel::deserialize(model.serialize());
    const dd::columns::Prediction before = model.predict("acct", {"1234"});
    const dd::columns::Prediction after = loaded.predict("acct", {"1234"});
    CHECK_EQ(before.label, after.label);
    CHECK_NEAR(before.confidence, after.confidence, 1e-12);
    CHECK_THROWS(dd::columns::ColumnModel::deserialize("{\"kind\":\"other\"}"));

    const auto model_json = [](const char* hyper, const char* classes, const char* params) {
        return std::string{"{\"kind\":\"column_transformer\",\"hyper\":"} + hyper +
               ",\"classes\":" + classes + ",\"params\":" + params + "}";
    };
    CHECK_THROWS(dd::columns::ColumnModel::deserialize(model_json(
        R"({"seq_len":-147,"d_model":8,"heads":1,"layers":1,"d_ffn":1})",
        R"(["a","b","c"])", "[0.1,0.2,0.3,0.4]")));
    CHECK_THROWS(dd::columns::ColumnModel::deserialize(model_json(
        R"({"seq_len":16,"d_model":8,"heads":0,"layers":1,"d_ffn":12})",
        R"(["a","b"])", "[0.1]")));
    CHECK_THROWS(dd::columns::ColumnModel::deserialize(model_json(
        R"({"seq_len":16,"d_model":8,"heads":3,"layers":1,"d_ffn":12})",
        R"(["a","b"])", "[0.1]")));
    CHECK_THROWS(dd::columns::ColumnModel::deserialize(model_json(
        R"({"d_model":8,"heads":2,"layers":1,"d_ffn":12})",
        R"(["a","b"])", "[0.1]")));
    CHECK_THROWS(dd::columns::ColumnModel::deserialize(model_json(
        R"({"seq_len":16,"d_model":8,"heads":2,"layers":1,"d_ffn":12})",
        R"(["a","a"])", "[0.1]")));
}

TEST(columns_training_rejects_degenerate_configuration) {
    std::vector<dd::columns::Example> one_class;
    for (int i = 0; i < 8; ++i) {
        one_class.push_back(dd::columns::Example{"col", {"1", "2"}, "only", "d"});
    }
    dd::columns::ColumnModel model{tiny_hyper()};
    dd::columns::TrainConfig config;
    config.epochs = 1;
    CHECK_THROWS(model.train(one_class, {}, config));

    std::vector<dd::columns::Example> two = synthetic_columns(4, 3);
    dd::columns::TrainConfig bad_batch;
    bad_batch.batch = 0;
    CHECK_THROWS(model.train(two, {}, bad_batch));

    std::vector<dd::columns::Example> train_out;
    std::vector<dd::columns::Example> holdout_out;
    CHECK_THROWS(dd::columns::split_by_domain(two, 1, &train_out, &holdout_out));
    CHECK_THROWS(dd::columns::split_by_domain(two, 4, nullptr, &holdout_out));
}

TEST(classifier_trains_with_high_holdout_accuracy) {
    dd::classify::TrainReport train_report;
    const dd::classify::Classifier classifier =
        dd::classify::Classifier::train_from_corpus("data/corpus", &train_report);
    CHECK_EQ(train_report.classes, std::size_t{8});
    CHECK(train_report.examples >= 40);
    CHECK(train_report.leave_one_out_accuracy >= 0.80);

    const std::string page =
        "<html><head><title>Overdue Property Tax Accounts</title></head><body>"
        "<h1>Delinquent Tax Accounts</h1>"
        "<p>Unpaid taxes accrue penalty and interest until redemption before the tax sale.</p>"
        "<table><tr><th>Parcel</th><th>Taxpayer</th><th>Amount Due</th><th>Tax Year</th></tr>"
        "<tr><td>661-04-118</td><td>Croft, Emily</td><td>$1,203.44</td><td>2025</td></tr>"
        "<tr><td>661-04-121</td><td>Marsden, Hugh</td><td>$477.10</td><td>2024</td></tr>"
        "</table></body></html>";
    const dd::doc::Model m = dd::doc::build_auto("text/html", page);
    const dd::classify::Prediction p = classifier.classify(m, "https://treasurer.example/delinquent");
    CHECK_EQ(p.label, "tax_delinquency");
    CHECK(p.confidence > 0.5);
    CHECK_EQ(p.distribution.size(), std::size_t{8});

    const std::string probate =
        "<html><head><title>Estate Docket</title></head><body><h1>Probate Filings</h1>"
        "<p>The decedent estates below have petitions for letters testamentary. The executor "
        "or personal representative administers the estate for the heirs.</p></body></html>";
    const dd::classify::Prediction p2 =
        classifier.classify(dd::doc::build_auto("text/html", probate), "");
    CHECK_EQ(p2.label, "probate_case");
}

TEST(classifier_save_load_roundtrip) {
    dd::classify::TrainReport train_report;
    const dd::classify::Classifier trained =
        dd::classify::Classifier::train_from_corpus("data/corpus", &train_report);
    const std::string path = "build/test_tmp/model.json";
    trained.save(path);
    const dd::classify::Classifier loaded = dd::classify::Classifier::load(path);
    CHECK_NEAR(loaded.trained_accuracy(), trained.trained_accuracy(), 1e-9);
    CHECK_EQ(loaded.example_count(), trained.example_count());

    const dd::doc::Model m = dd::doc::build_auto(
        "text/csv", "Permit No,Site Address,Description of Work,Valuation\n"
                    "B1,9 Elm St,garage electrical remodel inspection,12000\n"
                    "B2,4 Oak Ave,plumbing rough in permit,3300\n");
    const dd::classify::Prediction a = trained.classify(m, "");
    const dd::classify::Prediction b = loaded.classify(m, "");
    CHECK_EQ(a.label, b.label);
    CHECK_NEAR(a.confidence, b.confidence, 1e-9);
    CHECK_EQ(a.label, "building_permit");
}

TEST(classifier_missing_corpus_reports_error) {
    CHECK_THROWS(dd::classify::Classifier::train_from_corpus("data/no_such_dir", nullptr));
}

TEST(schema_money_parsing) {
    CHECK_NEAR(*dd::schema::parse_money("$8,421.37"), 8421.37, 1e-9);
    CHECK_NEAR(*dd::schema::parse_money("120"), 120.0, 1e-9);
    CHECK_NEAR(*dd::schema::parse_money("$ 1,000"), 1000.0, 1e-9);
    CHECK(!dd::schema::parse_money("Jane Smith").has_value());
    CHECK(!dd::schema::parse_money("12-34").has_value());
    CHECK(!dd::schema::parse_money("").has_value());
    CHECK_NEAR(*dd::schema::parse_money("($1,000)"), -1000.0, 1e-9);
    CHECK_NEAR(*dd::schema::parse_money("(500.25)"), -500.25, 1e-9);
    CHECK(!dd::schema::parse_money("(1,000").has_value());
    CHECK(!dd::schema::parse_money("1,000)").has_value());
}

TEST(schema_date_parsing) {
    CHECK_EQ(*dd::schema::parse_date("2026-06-18"), "2026-06-18");
    CHECK_EQ(*dd::schema::parse_date("06/18/2026"), "2026-06-18");
    CHECK_EQ(*dd::schema::parse_date("June 18, 2026"), "2026-06-18");
    CHECK_EQ(*dd::schema::parse_date("18 June 2026"), "2026-06-18");
    CHECK(!dd::schema::parse_date("Jane Smith").has_value());
    CHECK(!dd::schema::parse_date("123-456-789").has_value());
    CHECK(!dd::schema::parse_date("99/99/2026").has_value());
    CHECK(!dd::schema::parse_date("2026-02-31").has_value());
    CHECK(!dd::schema::parse_date("2026-04-31").has_value());
    CHECK_EQ(*dd::schema::parse_date("2024-02-29"), "2024-02-29");
    CHECK(!dd::schema::parse_date("2023-02-29").has_value());
    CHECK_EQ(*dd::schema::parse_date("2026-07-27T22:46:46.000"), "2026-07-27");
    CHECK_EQ(*dd::schema::parse_date("4/26/2018 12:00:00 AM"), "2018-04-26");
    CHECK(!dd::schema::parse_date("2026-02-11garbage").has_value());
    // A separator is not a licence for anything to follow it.
    CHECK(!dd::schema::parse_date("2026-02-11 garbage").has_value());
    CHECK(!dd::schema::parse_date("2026-02-11Tnot-a-time").has_value());
    CHECK(!dd::schema::parse_date("2026-02-11 99:99").has_value());
    CHECK_EQ(*dd::schema::parse_date("2026-02-11 09:30"), "2026-02-11");
    CHECK_EQ(*dd::schema::parse_date("2026-02-11T09:30:00.000"), "2026-02-11");
}

TEST(schema_validators) {
    CHECK(tvalidate("parcel_id", "123-456-789"));
    CHECK(tvalidate("parcel_id", "201-33-0870"));
    CHECK(!tvalidate("parcel_id", "Jane Smith"));
    CHECK(!tvalidate("parcel_id", "2026-06-18")); // a date is not a parcel
    CHECK(!tvalidate("parcel_id", "$1,200.00"));

    CHECK(tvalidate("owner", "Smith, Jane"));
    CHECK(!tvalidate("owner", "123-456"));

    CHECK(tvalidate("address", "1402 Main Street"));
    CHECK(tvalidate("address", "PO Box 118"));
    CHECK(!tvalidate("address", "8421.37"));

    CHECK(tvalidate("amount_due", "$8,421.37"));
    CHECK(!tvalidate("amount_due", "Jane"));

    CHECK(tvalidate("event_date", "2026-06-18"));
    CHECK(!tvalidate("event_date", "next week"));

    CHECK(tvalidate("case_number", "2026-CV-04182"));
    CHECK(!tvalidate("case_number", "$500"));

    CHECK(tvalidate("owner_email", "jlappy@gmail.com"));
    CHECK(tvalidate("owner_email", "JAIMEDELGADO19@YAHOO.COM"));
    CHECK(!tvalidate("owner_email", "17859 WILLIAM ST")); // a mailing address is not an email
    CHECK(!tvalidate("owner_email", "no-at-sign.com"));
    CHECK(!tvalidate("owner_email", "two@at@signs.com"));
    CHECK(!tvalidate("owner_email", "trailing@dot."));

    CHECK(tvalidate("owner_phone", "(407)353-6377"));
    CHECK(tvalidate("owner_phone", "+1 561 310 8909"));
    CHECK(!tvalidate("owner_phone", "22910 BURNHAM AVE")); // a street address is not a phone
    CHECK(!tvalidate("owner_phone", "555-1234"));          // too few digits
    CHECK(!tvalidate("owner_phone", "rosebert@gmail.com"));
}

TEST(schema_splits_a_composite_column_into_fields) {
    // One column carries two values; neither field has a column of its own.
    dd::doc::Model model;
    model.labels = {"parcel_id", "latitude_longitude"};
    for (const auto& [parcel, location] :
         std::vector<std::pair<std::string, std::string>>{{"101-22-0001", "36.930961, -76.201715"},
                                                          {"101-22-0002", "36.874750, -76.254110"},
                                                          {"101-22-0003", "36.901230, -76.221000"},
                                                          {"101-22-0004", "36.845600, -76.288900"}}) {
        dd::doc::RawRecord record;
        record.cells.push_back({"parcel_id", parcel});
        record.cells.push_back({"latitude_longitude", location});
        model.records.push_back(std::move(record));
    }

    const dd::schema::Mapping mapping = dd::schema::infer_mapping(test_registry(), model);
    const dd::schema::FieldMapping* lat = mapping.find("latitude");
    const dd::schema::FieldMapping* lon = mapping.find("longitude");
    CHECK(lat != nullptr);
    CHECK(lon != nullptr);
    // The same column feeds both fields, through different parts.
    CHECK_EQ(lat->source_label, std::string{"latitude_longitude"});
    CHECK_EQ(lon->source_label, std::string{"latitude_longitude"});
    CHECK(lat->part != lon->part);
    CHECK(lat->value_pass_rate > 0.9);

    const dd::schema::ExtractionResult extraction =
        dd::schema::apply_mapping(test_registry(), mapping, model);
    CHECK_EQ(extraction.records.size(), std::size_t{4});
    const auto& first = extraction.records[0].values;
    CHECK_EQ(first.at("latitude"), std::string{"36.930961"});
    CHECK_EQ(first.at("longitude"), std::string{"-76.201715"});
}

TEST(schema_split_mapping_survives_a_reload) {
    // Inference may map two parts of one column to two fields; if the format
    // cannot carry that back, the source works once and throws on reopen.
    dd::schema::Mapping mapping;
    mapping.fields.push_back({"latitude", "latitude_longitude", 1.0, 1.0, 1.0, true, 0});
    mapping.fields.push_back({"longitude", "latitude_longitude", 1.0, 1.0, 1.0, true, 1});
    mapping.confidence = 1.0;
    const dd::schema::Mapping back = dd::schema::Mapping::deserialize(mapping.serialize());
    CHECK_EQ(back.fields.size(), std::size_t{2});
    const dd::schema::FieldMapping* lat = back.find("latitude");
    const dd::schema::FieldMapping* lon = back.find("longitude");
    CHECK(lat != nullptr);
    CHECK(lon != nullptr);
    CHECK_EQ(lat->source_label, lon->source_label);
    CHECK(lat->part != lon->part);

    // The same label and the same part twice is still a duplicate.
    dd::schema::Mapping clashing;
    clashing.fields.push_back({"latitude", "x", 1.0, 1.0, 1.0, true, 0});
    clashing.fields.push_back({"longitude", "x", 1.0, 1.0, 1.0, true, 0});
    clashing.confidence = 1.0;
    CHECK_THROWS(dd::schema::Mapping::deserialize(clashing.serialize()));
}

TEST(schema_refuses_to_split_ragged_prose) {
    // A column that splits differently per row is prose, not a composite.
    dd::doc::Model model;
    model.labels = {"parcel_id", "notes"};
    for (const auto& [parcel, note] :
         std::vector<std::pair<std::string, std::string>>{
             {"101-22-0001", "sold, vacant, boarded"},
             {"101-22-0002", "occupied"},
             {"101-22-0003", "lien, filed"},
             {"101-22-0004", "36.9, -76.2, extra, more"}}) {
        dd::doc::RawRecord record;
        record.cells.push_back({"parcel_id", parcel});
        record.cells.push_back({"notes", note});
        model.records.push_back(std::move(record));
    }
    const dd::schema::Mapping mapping = dd::schema::infer_mapping(test_registry(), model);
    CHECK(mapping.find("latitude") == nullptr);
    CHECK(mapping.find("longitude") == nullptr);
}

TEST(schema_normalization) {
    CHECK_EQ(tnormalize("amount_due", "$8,421.37"), "8421.37");
    CHECK_EQ(tnormalize("event_date", "06/18/2026"), "2026-06-18");
    CHECK_EQ(tnormalize("parcel_id", "12a-33"), "12A-33");
    CHECK_EQ(tnormalize("owner", "  Jane   Smith "), "Jane Smith");
    CHECK_EQ(tnormalize("owner_email", " JAIMEDELGADO19@YAHOO.COM "), "jaimedelgado19@yahoo.com");
    CHECK_EQ(tnormalize("owner_phone", " (407)353-6377 "), "(407)353-6377");
}

TEST(schema_infers_mapping_across_dialects) {
    const dd::doc::Model a = dd::doc::build_auto(
        "text/csv",
        "Parcel Number,Owner Name,Property Address,Amount Due,Sale Date\n"
        "111-22-333,\"Smith, Jane\",19 Birch Ln,\"$2,114.90\",2026-08-01\n"
        "444-55-666,\"Ray, Bob\",820 Canal Rd,$860.02,2026-08-01\n");
    const dd::schema::Mapping ma = dd::schema::infer_mapping(test_registry(), a);
    CHECK(ma.find("parcel_id") != nullptr);
    CHECK_EQ(ma.find("parcel_id")->source_label, "Parcel Number");
    CHECK(ma.find("owner") != nullptr);
    CHECK(ma.find("address") != nullptr);
    CHECK(ma.find("amount_due") != nullptr);
    CHECK(ma.find("auction_date") != nullptr);
    CHECK(ma.confidence > 0.7);

    const dd::doc::Model b = dd::doc::build_auto(
        "application/json",
        R"({"rows": [
            {"apn": "77-100-08", "taxpayer": "Nguyen, An", "situs": "12 Fern Way", "balance": 902.11},
            {"apn": "77-100-31", "taxpayer": "Cole, Dana", "situs": "77 Mill St", "balance": 5210.40}
        ]})");
    const dd::schema::Mapping mb = dd::schema::infer_mapping(test_registry(), b);
    CHECK(mb.find("parcel_id") != nullptr);
    CHECK_EQ(mb.find("parcel_id")->source_label, "apn");
    CHECK(mb.find("owner") != nullptr);
    CHECK_EQ(mb.find("owner")->source_label, "taxpayer");
    CHECK(mb.find("address") != nullptr);
    CHECK(mb.find("amount_due") != nullptr);
    CHECK_EQ(mb.find("amount_due")->source_label, "balance");
}

TEST(schema_mapping_requires_identity_field) {
    const dd::doc::Model m = dd::doc::build_auto(
        "text/csv", "Amount,Date\n$100,2026-01-01\n$200,2026-01-02\n$300,2026-01-03\n");
    const dd::schema::Mapping mapping = dd::schema::infer_mapping(test_registry(), m);
    CHECK(mapping.fields.empty());
    CHECK_NEAR(mapping.confidence, 0.0, 1e-12);
}

TEST(schema_mapping_confidence_reflects_measurements) {
    const dd::doc::Model m = dd::doc::build_auto(
        "text/csv",
        "Parcel,Owner\n111-22,Jane\n444-55,Bob\n777-88,Sue\n");
    const dd::schema::Mapping mapping = dd::schema::infer_mapping(test_registry(), m);
    const dd::schema::FieldMapping* parcel = mapping.find("parcel_id");
    CHECK(parcel != nullptr);
    CHECK_NEAR(parcel->value_pass_rate, 1.0, 1e-9);
    CHECK_NEAR(parcel->confidence,
               0.55 * parcel->label_similarity + 0.45 * parcel->value_pass_rate, 1e-9);
}

TEST(schema_apply_mapping_measures_rates) {
    const dd::doc::Model m = dd::doc::build_auto(
        "text/csv",
        "Parcel,Owner,Amount Due\n"
        "111-22,Jane,$100.00\n"
        "444-55,Bob,not available\n" // invalid money: must count against the rate
        "777-88,Sue,$300.00\n");
    const dd::schema::Mapping mapping = dd::schema::infer_mapping(test_registry(), m);
    const dd::schema::ExtractionResult result = dd::schema::apply_mapping(test_registry(), mapping, m);
    CHECK_EQ(result.records.size(), std::size_t{3});
    CHECK_EQ(result.records[0].values.at("amount_due"), "100.00");
    CHECK(result.records[1].values.find("amount_due") == result.records[1].values.end());
    CHECK_NEAR(result.field_rates.at("amount_due"), 2.0 / 3.0, 1e-9);
    CHECK_NEAR(result.field_rates.at("parcel_id"), 1.0, 1e-9);
    CHECK(result.rate > 0.5);
    CHECK(result.rate < 1.0);
}

TEST(schema_mapping_serialize_roundtrip) {
    const dd::doc::Model m = dd::doc::build_auto(
        "text/csv", "Parcel,Owner,Balance\n111-22,Jane,$10\n444-55,Bob,$20\n");
    const dd::schema::Mapping mapping = dd::schema::infer_mapping(test_registry(), m);
    const dd::schema::Mapping loaded = dd::schema::Mapping::deserialize(mapping.serialize());
    CHECK_EQ(loaded.fields.size(), mapping.fields.size());
    CHECK_NEAR(loaded.confidence, mapping.confidence, 1e-12);
    const dd::schema::FieldMapping* owner = loaded.find("owner");
    CHECK(owner != nullptr);
    CHECK_EQ(owner->source_label, "Owner");
}

TEST(entity_normalization) {
    CHECK_EQ(dd::entity::normalize_parcel("123-456-789"), "123456789");
    CHECK_EQ(dd::entity::normalize_parcel(" 12a.33 "), "12A33");
    CHECK_EQ(dd::entity::normalize_address("1402 North Main Street"),
             dd::entity::normalize_address("1402 N MAIN ST"));
    CHECK(dd::entity::normalize_address("19 Birch Ln") !=
          dd::entity::normalize_address("21 Birch Ln"));
}

TEST(entity_property_key) {
    const std::string with_parcel =
        dd::entity::property_key("Hamilton County", "123-456", "19 Birch Ln");
    CHECK_EQ(with_parcel, "hamilton_county|p:123456");
    const std::string with_address = dd::entity::property_key("Hamilton County", "", "19 Birch Lane");
    CHECK_EQ(with_address, "hamilton_county|a:19 birch ln");
    CHECK(dd::entity::property_key("X", "", "").empty());
    CHECK(dd::entity::property_key("A", "123", "") != dd::entity::property_key("B", "123", ""));
}

TEST(entity_same_owner) {
    CHECK(dd::entity::same_owner("Smith, Jane", "Jane Smith"));
    CHECK(dd::entity::same_owner("SMITH JANE", "jane smith"));
    CHECK(!dd::entity::same_owner("Jane Smith", "Bob Ray"));
    CHECK(!dd::entity::same_owner("", "Bob Ray"));
}

dd::events::PropertyEvent make_event(dd::events::Kind kind, const std::string& date,
                                     const std::string& source = "s1") {
    dd::events::PropertyEvent e;
    e.property_key = "county|p:123";
    e.kind = kind;
    e.event_date = date;
    e.recorded_at = "2026-07-01T00:00:00Z";
    e.source_id = source;
    e.run_id = "r1";
    e.confidence = 0.9;
    e.id = dd::events::PropertyEvent::compute_id(e);
    return e;
}

TEST(events_kind_from_source) {
    using dd::events::Kind;
    CHECK(dd::events::kind_from_source_label("tax_delinquency", "") == Kind::TaxDelinquency);
    CHECK(dd::events::kind_from_source_label("trustee_auction", "scheduled") ==
          Kind::AuctionScheduled);
    CHECK(dd::events::kind_from_source_label("trustee_auction", "sold at auction") ==
          Kind::SoldAtAuction);
    CHECK(dd::events::kind_from_source_label("deed_transfer", "") == Kind::DeedTransfer);
}

TEST(events_lifecycle_advances_in_order) {
    using dd::events::Kind;
    using dd::events::State;
    std::vector<dd::events::PropertyEvent> evs = {
        make_event(Kind::AuctionScheduled, "2026-05-01"),
        make_event(Kind::TaxDelinquency, "2026-01-01"),
        make_event(Kind::ForeclosureFiled, "2026-03-01"),
    };
    const dd::events::Lifecycle life = dd::events::reduce(evs);
    CHECK(life.state == State::AuctionScheduled);
    CHECK_EQ(life.transitions.size(), std::size_t{3});
    CHECK(life.transitions[0].state == State::TaxDelinquent);
    CHECK(life.transitions[1].state == State::ForeclosureFiled);
    CHECK(life.transitions[2].state == State::AuctionScheduled);
}

TEST(events_lifecycle_never_regresses) {
    using dd::events::Kind;
    using dd::events::State;
    std::vector<dd::events::PropertyEvent> evs = {
        make_event(Kind::ForeclosureFiled, "2026-03-01"),
        make_event(Kind::TaxDelinquency, "2026-04-01"), // later but lower rank
    };
    const dd::events::Lifecycle life = dd::events::reduce(evs);
    CHECK(life.state == State::ForeclosureFiled);
    CHECK_EQ(life.transitions.size(), std::size_t{1});
}

TEST(events_deed_transfer_resets) {
    using dd::events::Kind;
    using dd::events::State;
    std::vector<dd::events::PropertyEvent> evs = {
        make_event(Kind::TaxDelinquency, "2026-01-01"),
        make_event(Kind::SoldAtAuction, "2026-05-01"),
        make_event(Kind::DeedTransfer, "2026-06-01"),
    };
    const dd::events::Lifecycle life = dd::events::reduce(evs);
    CHECK(life.state == State::Normal);
    CHECK_EQ(life.transitions.back().state, State::Normal);
}

TEST(events_evidence_kinds_do_not_move_state) {
    using dd::events::Kind;
    using dd::events::State;
    std::vector<dd::events::PropertyEvent> evs = {
        make_event(Kind::PermitIssued, "2026-01-01"),
        make_event(Kind::CodeViolation, "2026-02-01"),
        make_event(Kind::AssessmentRecorded, "2026-03-01"),
        make_event(Kind::ProbateOpened, "2026-04-01"),
    };
    const dd::events::Lifecycle life = dd::events::reduce(evs);
    CHECK(life.state == State::Normal);
    CHECK(life.transitions.empty());
}

TEST(events_id_is_content_stable) {
    const dd::events::PropertyEvent a = make_event(dd::events::Kind::TaxDelinquency, "2026-01-01");
    dd::events::PropertyEvent b = make_event(dd::events::Kind::TaxDelinquency, "2026-01-01");
    b.run_id = "another_run";
    b.recorded_at = "2026-08-01T00:00:00Z";
    b.id = dd::events::PropertyEvent::compute_id(b);
    CHECK_EQ(a.id, b.id); // same fact, different run: same identity
    const dd::events::PropertyEvent c = make_event(dd::events::Kind::TaxDelinquency, "2026-02-01");
    CHECK(a.id != c.id);
}

TEST(events_serialize_roundtrip) {
    dd::events::PropertyEvent e = make_event(dd::events::Kind::ForeclosureFiled, "2026-03-01");
    e.amount = 214880.15;
    e.details["case_number"] = "26-FC-1108";
    e.details["owner"] = "Marsh, Tobias";
    const dd::events::PropertyEvent back = dd::events::PropertyEvent::deserialize(e.serialize());
    CHECK_EQ(back.id, e.id);
    CHECK(back.kind == e.kind);
    CHECK_NEAR(back.amount, e.amount, 1e-9);
    CHECK_EQ(back.details.at("case_number"), "26-FC-1108");
    CHECK_THROWS(dd::events::PropertyEvent::deserialize("{\"kind\":\"nope\"}"));
}

std::string fresh_dir(const std::string& name) {
    const std::string dir = "build/test_tmp/" + name;
    std::filesystem::remove_all(dir);
    dd::fileio::ensure_dir(dir);
    return dir;
}

TEST(columns_corpus_roundtrips_jsonl) {
    const std::string dir = fresh_dir("columns_corpus");
    const std::string path = dir + "/corpus.jsonl";
    std::vector<dd::columns::Example> examples = synthetic_columns(2, 1);
    dd::columns::append_corpus(path, examples);
    dd::columns::append_corpus(path, {examples.front()});
    const std::vector<dd::columns::Example> loaded = dd::columns::load_corpus(path);
    CHECK_EQ(loaded.size(), examples.size() + 1);
    CHECK_EQ(loaded.front().name, examples.front().name);
    CHECK_EQ(loaded.front().values.size(), std::size_t{3});
    CHECK_EQ(loaded.front().domain, examples.front().domain);
}

TEST(bench_golden_loads_strings_and_lists) {
    const std::string dir = fresh_dir("bench_golden");
    const std::string path = dir + "/golden.json";
    dd::fileio::write_file_atomic(path, R"({"sources": [
        {"id": "one", "classification": "tax_delinquency",
         "fields": {"parcel_id": "pin", "amount_due": ["total", ""]}},
        {"id": "two", "classification": ["a", "b"]}
    ]})");
    const std::vector<dd::bench::Golden> golden = dd::bench::load_golden(path);
    CHECK_EQ(golden.size(), std::size_t{2});
    CHECK_EQ(golden[0].fields.at("parcel_id").size(), std::size_t{1});
    CHECK_EQ(golden[0].fields.at("amount_due")[1], "");
    CHECK_EQ(golden[1].classifications.size(), std::size_t{2});
    CHECK(golden[1].fields.empty());

    CHECK_THROWS(dd::bench::load_golden(dir + "/absent.json"));
    dd::fileio::write_file_atomic(path, R"({"sources": [{"id": "x"}]})");
    CHECK_THROWS(dd::bench::load_golden(path));
    dd::fileio::write_file_atomic(path, R"({"sources": [
        {"id": "dup", "classification": "a"}, {"id": "dup", "classification": "b"}]})");
    CHECK_THROWS(dd::bench::load_golden(path));
    dd::fileio::write_file_atomic(path, R"({"sources": [{"id": "", "classification": "a"}]})");
    CHECK_THROWS(dd::bench::load_golden(path));
}

TEST(store_sources_persist_across_reopen) {
    const std::string root = fresh_dir("store_sources");
    {
        dd::store::Store store{root};
        CHECK(store.sources().empty());
        const dd::store::Source s = store.add_source("Test County", "data/fixtures/crestline_auctions.csv", "Test County");
        CHECK(!s.id.empty());
        CHECK(store.find_source(s.id).has_value());
    }
    {
        dd::store::Store reopened{root};
        CHECK_EQ(reopened.sources().size(), std::size_t{1});
        CHECK_EQ(reopened.sources()[0].name, "Test County");
    }
    dd::store::Store store{root};
    CHECK_THROWS(store.add_source("", "url", ""));
    CHECK_THROWS(store.add_source("name", " ", ""));
}

TEST(store_seed_creates_sources_and_working_copies) {
    const std::string root = fresh_dir("store_seed");
    const std::string seeds = root + "/seeds.json";
    dd::fileio::write_file_atomic(
        seeds, "[{\"id\":\"demo\",\"name\":\"Demo\",\"url\":\"" + root +
                   "/local/demo.html\",\"jurisdiction\":\"Demo County\","
                   "\"seed_from\":\"data/fixtures/millbrook_tax.html\"}]");
    dd::store::Store store{root};
    store.seed(seeds);
    CHECK_EQ(store.sources().size(), std::size_t{1});
    CHECK(dd::fileio::exists(root + "/local/demo.html"));
    store.seed(seeds);
    CHECK_EQ(store.sources().size(), std::size_t{1});
}

TEST(store_runs_events_repairs_roundtrip) {
    const std::string root = fresh_dir("store_rounds");
    dd::store::Store store{root};

    dd::store::RunRecord run;
    run.id = "r1";
    run.source_id = "s1";
    run.started_at = "2026-07-01T00:00:00Z";
    run.ok = true;
    run.stage = "done";
    run.bytes = 1234;
    run.fetch_ms = 1.5;
    run.extraction_rate = 0.9;
    store.record_run(run);

    dd::events::PropertyEvent e = make_event(dd::events::Kind::TaxDelinquency, "2026-01-01");
    CHECK_EQ(store.add_events({e, e}), std::size_t{1}); // same id twice: one insert
    CHECK_EQ(store.add_events({e}), std::size_t{0});    // and never again

    dd::store::RepairRecord repair;
    repair.id = "rep1";
    repair.source_id = "s1";
    repair.at = "2026-07-01T00:00:00Z";
    repair.reason = "test";
    repair.confidence = 0.8;
    repair.accepted = true;
    repair.changes = {"owner: 'a' -> 'b'"};
    store.add_repair(repair);

    dd::store::Store reopened{root};
    const std::vector<dd::store::RunRecord> runs = reopened.runs(10);
    CHECK_EQ(runs.size(), std::size_t{1});
    CHECK_EQ(runs[0].bytes, std::int64_t{1234});
    CHECK_NEAR(runs[0].extraction_rate, 0.9, 1e-9);
    CHECK_EQ(reopened.event_count(), std::size_t{1});
    CHECK_EQ(reopened.events_for(e.property_key).size(), std::size_t{1});
    const std::vector<dd::store::RepairRecord> repairs = reopened.repairs();
    CHECK_EQ(repairs.size(), std::size_t{1});
    CHECK_EQ(repairs[0].changes.size(), std::size_t{1});
    CHECK(repairs[0].accepted);
}

dd::classify::Classifier test_classifier() {
    return dd::classify::Classifier::load("data/model/source_classifier.json");
}

TEST(pipeline_learns_and_ingests_table_site) {
    const std::string root = fresh_dir("pipe_learn");
    dd::store::Store store{root};
    const dd::store::Source source =
        store.add_source("Millbrook Tax", "data/fixtures/millbrook_tax.html", "Millbrook County");
    dd::pipeline::Pipeline pipeline{store, test_classifier(), test_registry()};

    const dd::store::RunRecord run = pipeline.run_source(source);
    CHECK(run.ok);
    CHECK_EQ(run.stage, "done");
    CHECK_EQ(run.classification, "tax_delinquency");
    CHECK(run.class_confidence > 0.5);
    CHECK_EQ(run.records_extracted, std::int64_t{6});
    CHECK_EQ(run.events_new, std::int64_t{6});
    CHECK(run.extraction_rate > 0.8);
    CHECK(run.mapping_confidence > 0.7);
    CHECK(run.bytes > 1000);
    CHECK(run.fetch_ms >= 0.0);
    CHECK(run.total_ms > 0.0);
    CHECK(run.rss_bytes > 1024 * 1024);
    CHECK(!run.structure_fingerprint.empty());

    const dd::store::SourceState state = store.source_state(source.id);
    CHECK(state.has_mapping);
    CHECK_NEAR(state.baseline_rate, run.extraction_rate, 1e-9);
    CHECK_EQ(state.good_runs, 1);

    const std::vector<std::string> keys = store.property_keys();
    CHECK_EQ(keys.size(), std::size_t{6});
    CHECK(dd::str::contains(keys[0], "millbrook_county|p:"));

    const dd::store::RunRecord again = pipeline.run_source(source);
    CHECK(again.ok);
    CHECK_EQ(again.events_new, std::int64_t{0});
    CHECK(!again.drift_detected);
}

TEST(pipeline_handles_json_csv_pdf_dialects) {
    const std::string root = fresh_dir("pipe_dialects");
    dd::store::Store store{root};
    dd::pipeline::Pipeline pipeline{store, test_classifier(), test_registry()};

    const dd::store::Source foreclosures = store.add_source(
        "Harborview FC", "data/fixtures/harborview_foreclosures.json", "Harborview County");
    const dd::store::RunRecord fc = pipeline.run_source(foreclosures);
    CHECK(fc.ok);
    CHECK_EQ(fc.classification, "foreclosure_filing");
    CHECK_EQ(fc.format, "json");
    CHECK_EQ(fc.events_new, std::int64_t{4});

    const dd::store::Source auctions = store.add_source(
        "Crestline Auctions", "data/fixtures/crestline_auctions.csv", "Crestline County");
    const dd::store::RunRecord au = pipeline.run_source(auctions);
    CHECK(au.ok);
    CHECK_EQ(au.classification, "trustee_auction");
    CHECK_EQ(au.format, "csv");
    CHECK_EQ(au.events_new, std::int64_t{4});

    const dd::store::Source pdf = store.add_source(
        "Eldridge PDF", "data/fixtures/eldridge_delinquent.pdf", "Eldridge County");
    const dd::store::RunRecord pd = pipeline.run_source(pdf);
    CHECK(pd.ok);
    CHECK_EQ(pd.classification, "tax_delinquency");
    CHECK_EQ(pd.format, "pdf");
    CHECK_EQ(pd.records_extracted, std::int64_t{4});

    bool found_sold = false;
    for (const std::string& key : store.property_keys()) {
        const std::vector<dd::events::PropertyEvent> evs = store.events_for(key);
        const dd::events::Lifecycle life = dd::events::reduce(evs);
        if (life.state == dd::events::State::SoldAtAuction) found_sold = true;
    }
    CHECK(found_sold);
}

TEST(pipeline_records_fetch_failure) {
    const std::string root = fresh_dir("pipe_fail");
    dd::store::Store store{root};
    const dd::store::Source source =
        store.add_source("Broken", "build/test_tmp/nope_not_here.html", "Nowhere");
    dd::pipeline::Pipeline pipeline{store, test_classifier(), test_registry()};
    const dd::store::RunRecord run = pipeline.run_source(source);
    CHECK(!run.ok);
    CHECK_EQ(run.stage, "fetch");
    CHECK(!run.error.empty());
    CHECK_EQ(store.runs(5).size(), std::size_t{1});
}

TEST(pipeline_detects_drift_and_heals) {
    const std::string root = fresh_dir("pipe_drift");
    dd::store::Store store{root};
    const std::string site = root + "/local/site.html";

    dd::fileio::write_file_atomic(site, dd::fileio::read_file("data/fixtures/millbrook_tax.html"));
    const dd::store::Source source = store.add_source("Millbrook Drift", site, "Millbrook County");
    dd::pipeline::Pipeline pipeline{store, test_classifier(), test_registry()};

    const dd::store::RunRecord first = pipeline.run_source(source);
    CHECK(first.ok);
    CHECK_EQ(first.events_new, std::int64_t{6});
    const std::string old_fingerprint = first.structure_fingerprint;

    dd::fileio::write_file_atomic(site,
                                  dd::fileio::read_file("data/fixtures/millbrook_tax_v2.html"));
    const dd::store::RunRecord second = pipeline.run_source(source);
    CHECK(second.ok);
    CHECK(second.drift_detected);
    CHECK(second.repair_attempted);
    CHECK(second.repair_accepted);
    CHECK(second.structure_fingerprint != old_fingerprint);
    CHECK(second.extraction_rate > 0.8);
    CHECK_EQ(second.events_new, std::int64_t{1});

    const std::vector<dd::store::RepairRecord> repairs = store.repairs(source.id);
    CHECK_EQ(repairs.size(), std::size_t{1});
    CHECK(repairs[0].accepted);
    CHECK(repairs[0].confidence >= dd::heal::auto_accept_threshold());
    CHECK(repairs[0].after_rate > 0.8);
    bool mentions_owner = false;
    for (const std::string& change : repairs[0].changes) {
        if (dd::str::contains(change, "owner") && dd::str::contains(change, "taxpayer")) {
            mentions_owner = true;
        }
    }
    CHECK(mentions_owner);

    const dd::store::RunRecord third = pipeline.run_source(source);
    CHECK(third.ok);
    CHECK(!third.drift_detected);
    CHECK_EQ(third.events_new, std::int64_t{0});
}

TEST(heal_assessment_ignores_healthy_updates) {
    const dd::doc::Model m = dd::doc::build_auto(
        "text/csv", "Parcel,Owner\n111-22,Jane\n444-55,Bob\n777-88,Sue\n");
    const dd::schema::Mapping mapping = dd::schema::infer_mapping(test_registry(), m);
    const dd::schema::ExtractionResult extraction = dd::schema::apply_mapping(test_registry(), mapping, m);

    dd::store::SourceState state;
    state.source_id = "s";
    state.has_mapping = true;
    state.mapping = mapping;
    state.baseline_rate = extraction.rate;
    state.good_runs = 3;
    state.fingerprint = "different_fingerprint";

    const dd::heal::Assessment verdict = dd::heal::assess(state, m, extraction);
    CHECK(!verdict.drift);
    CHECK(verdict.fingerprint_changed);
}

TEST(model_summaries_surface_discriminative_vocabulary) {
    const dd::classify::Classifier classifier = test_classifier();
    const std::vector<dd::model::ClassSummary> classes = classifier.bayes().summarize(10);
    CHECK_EQ(classes.size(), std::size_t{8});
    bool tax_has_delinquent = false;
    for (const dd::model::ClassSummary& c : classes) {
        CHECK(c.documents >= 5);
        CHECK(!c.top_tokens.empty());
        if (c.name == "tax_delinquency") {
            for (const dd::model::TokenWeight& t : c.top_tokens) {
                if (dd::str::contains(t.token, "delinquen") || dd::str::contains(t.token, "tax")) {
                    tax_has_delinquent = true;
                }
                CHECK(t.lift > 1.0); // above corpus-average frequency by definition
            }
        }
    }
    CHECK(tax_has_delinquent);
}

TEST(pipeline_resolves_across_sources) {
    const std::string root = fresh_dir("pipe_relations");
    dd::store::Store store{root};
    dd::pipeline::Pipeline pipeline{store, test_classifier(), test_registry()};

    const dd::store::Source tax =
        store.add_source("Millbrook Tax", "data/fixtures/millbrook_tax.html", "Millbrook County");
    const dd::store::Source assessor = store.add_source(
        "Millbrook Assessor", "data/fixtures/millbrook_assessor.html", "Millbrook County");
    CHECK(pipeline.run_source(tax).ok);
    const dd::store::RunRecord second = pipeline.run_source(assessor);
    CHECK(second.ok);
    CHECK_EQ(second.classification, "assessor_roll");

    const std::vector<std::string> keys = store.property_keys();
    CHECK_EQ(keys.size(), std::size_t{6});
    for (const std::string& key : keys) {
        const std::vector<dd::events::PropertyEvent> evs = store.events_for(key);
        CHECK_EQ(evs.size(), std::size_t{2});
        CHECK(evs[0].source_id != evs[1].source_id);
        CHECK(dd::events::reduce(evs).state == dd::events::State::TaxDelinquent);
    }
}

TEST(fetch_encodes_spaces_in_urls) {
    const dd::fetch::Result r = dd::fetch::get("gopher://x/a b");
    CHECK(!dd::str::contains(r.error, "Malformed"));
}

TEST(snapshot_carries_raw_dialect_sample) {
    const std::string root = fresh_dir("pipe_snapshot");
    dd::store::Store store{root};
    dd::pipeline::Pipeline pipeline{store, test_classifier(), test_registry()};
    const dd::store::Source source =
        store.add_source("Millbrook", "data/fixtures/millbrook_tax.html", "Millbrook County");
    CHECK(pipeline.run_source(source).ok);
    const dd::json::Value snap = dd::json::parse(store.latest_records(source.id));
    const dd::json::Value* raw = snap.find("raw_sample");
    CHECK(raw != nullptr && raw->is_array());
    CHECK(raw->items().size() >= 5);
    CHECK_EQ(raw->items()[0].find("label")->as_string(), "Parcel Number");
    CHECK_EQ(raw->items()[0].find("value")->as_string(), "04-118-002");
}

TEST(store_update_and_remove_source) {
    const std::string root = fresh_dir("store_update");
    dd::store::Store store{root};
    const dd::store::Source s =
        store.add_source("Millbrook Tax", "data/fixtures/millbrook_tax.html", "Millbrook County");

    dd::store::SourceUpdate update;
    update.name = "Millbrook Treasurer";
    update.enabled = false;
    const dd::store::Source updated = store.update_source(s.id, update);
    CHECK_EQ(updated.name, "Millbrook Treasurer");
    CHECK_EQ(updated.url, s.url);
    CHECK(!updated.enabled);
    {
        dd::store::Store reopened{root};
        CHECK_EQ(reopened.find_source(s.id)->name, "Millbrook Treasurer");
        CHECK(!reopened.find_source(s.id)->enabled);
    }
    CHECK_THROWS(store.update_source("nope", update));
    dd::store::SourceUpdate blank;
    blank.url = "  ";
    CHECK_THROWS(store.update_source(s.id, blank));

    dd::pipeline::Pipeline pipeline{store, test_classifier(), test_registry()};
    CHECK(pipeline.run_source(*store.find_source(s.id)).ok);
    CHECK(dd::fileio::exists(root + "/state/" + s.id + ".json"));
    CHECK(dd::fileio::exists(root + "/records/" + s.id + ".json"));
    CHECK(dd::fileio::exists(root + "/cache/" + s.id));
    const std::size_t events_before = store.event_count();
    CHECK(events_before > 0);
    store.remove_source(s.id);
    CHECK(!store.find_source(s.id).has_value());
    CHECK(!dd::fileio::exists(root + "/state/" + s.id + ".json"));
    CHECK(!dd::fileio::exists(root + "/records/" + s.id + ".json"));
    CHECK(!dd::fileio::exists(root + "/cache/" + s.id));
    CHECK(!dd::fileio::exists(root + "/cache/" + s.id + ".meta"));
    CHECK_EQ(store.runs(10).size(), std::size_t{1});
    CHECK_EQ(store.event_count(), events_before);
    CHECK_THROWS(store.remove_source(s.id));
}

TEST(repair_resolution_serialization_and_back_compat) {
    const dd::store::RepairRecord legacy_auto = dd::store::RepairRecord::deserialize(
        R"({"id":"r1","source_id":"s1","at":"2026-07-01T00:00:00Z","accepted":true})");
    CHECK_EQ(legacy_auto.resolution, "auto");
    const dd::store::RepairRecord legacy_pending = dd::store::RepairRecord::deserialize(
        R"({"id":"r2","source_id":"s1","at":"2026-07-01T00:00:00Z","accepted":false})");
    CHECK_EQ(legacy_pending.resolution, "pending");

    dd::store::RepairRecord repair;
    repair.id = "r3";
    repair.source_id = "s1";
    repair.resolution = "approved";
    const dd::store::RepairRecord back =
        dd::store::RepairRecord::deserialize(repair.serialize());
    CHECK_EQ(back.resolution, "approved");
}

TEST(store_resolve_repair_applies_mapping) {
    const std::string root = fresh_dir("store_resolve");
    dd::store::Store store{root};
    const dd::store::Source s =
        store.add_source("Millbrook Tax", "data/fixtures/millbrook_tax.html", "Millbrook County");
    dd::pipeline::Pipeline pipeline{store, test_classifier(), test_registry()};
    CHECK(pipeline.run_source(s).ok);
    const dd::store::SourceState before = store.source_state(s.id);
    CHECK(before.has_mapping);

    const dd::doc::Model v2 = dd::doc::build_auto(
        "text/html", dd::fileio::read_file("data/fixtures/millbrook_tax_v2.html"));
    const dd::schema::Mapping v2_mapping = dd::schema::infer_mapping(test_registry(), v2);
    CHECK(!v2_mapping.fields.empty());
    dd::store::RepairRecord pending;
    pending.id = "pending_1";
    pending.source_id = s.id;
    pending.at = "2026-07-01T00:00:00Z";
    pending.reason = "test drift";
    pending.before_mapping_json = before.mapping.serialize();
    pending.after_mapping_json = v2_mapping.serialize();
    pending.resolution = "pending";
    store.add_repair(pending);

    const dd::store::RepairRecord approved = store.resolve_repair("pending_1", true);
    CHECK_EQ(approved.resolution, "approved");
    const dd::store::SourceState after = store.source_state(s.id);
    CHECK(after.has_mapping);
    CHECK_EQ(after.good_runs, 0); // baseline restarts under the new mapping
    const dd::schema::FieldMapping* owner = after.mapping.find("owner");
    CHECK(owner != nullptr);
    CHECK_EQ(owner->source_label, "taxpayer");

    dd::store::Store reopened{root};
    CHECK_EQ(reopened.repairs(s.id).front().resolution, "approved");
    CHECK_THROWS(store.resolve_repair("pending_1", false));
    CHECK_THROWS(store.resolve_repair("no_such_repair", true));
}

TEST(pipeline_applies_operator_overrides) {
    const std::string root = fresh_dir("pipe_overrides");
    dd::store::Store store{root};
    dd::pipeline::Pipeline pipeline{store, test_classifier(), test_registry()};
    const dd::store::Source source =
        store.add_source("Millbrook", "data/fixtures/millbrook_tax.html", "Millbrook County");
    CHECK(pipeline.run_source(source).ok);

    dd::store::SourceState state = store.source_state(source.id);
    state.overrides["auction_date"] = "";
    state.overrides["owner"] = "Property Address";
    store.save_source_state(state);

    const dd::store::RunRecord run = pipeline.run_source(source);
    CHECK(run.ok);
    const dd::json::Value snap = dd::json::parse(store.latest_records(source.id));
    const dd::json::Value* fields = snap.find("mapping")->find("fields");
    bool has_auction = false;
    std::string owner_label;
    for (const dd::json::Value& f : fields->items()) {
        if (f.find("field")->as_string() == "auction_date") has_auction = true;
        if (f.find("field")->as_string() == "owner") {
            owner_label = f.find("source_label")->as_string();
        }
    }
    CHECK(!has_auction);
    CHECK_EQ(owner_label, "Property Address");

    dd::store::Store reopened{root};
    CHECK_EQ(reopened.source_state(source.id).overrides.at("owner"), "Property Address");
}

TEST(classify_detailed_training_report) {
    dd::classify::TrainReport train_report;
    const dd::classify::Classifier trained =
        dd::classify::Classifier::train_from_corpus("data/corpus", &train_report);
    const std::vector<dd::classify::LooPrediction>& predictions = train_report.predictions;
    CHECK_EQ(predictions.size(), train_report.examples);
    std::size_t correct = 0;
    for (const dd::classify::LooPrediction& p : predictions) {
        CHECK(!p.actual.empty());
        CHECK(!p.predicted.empty());
        if (p.actual == p.predicted) ++correct;
    }
    CHECK_NEAR(static_cast<double>(correct) / static_cast<double>(predictions.size()),
               train_report.leave_one_out_accuracy, 1e-9);
}

TEST(pipeline_replays_cached_fetch) {
    const std::string root = fresh_dir("pipe_replay");
    dd::store::Store store{root};
    dd::pipeline::Pipeline pipeline{store, test_classifier(), test_registry()};
    const dd::store::Source source =
        store.add_source("Millbrook", "data/fixtures/millbrook_tax.html", "Millbrook County");
    CHECK(pipeline.run_source(source).ok);
    CHECK(store.has_fetch_cache(source.id));

    const dd::store::RunRecord replay = pipeline.run_cached(source);
    CHECK(replay.ok);
    CHECK_EQ(replay.records_extracted, std::int64_t{6});
    CHECK_EQ(replay.events_new, std::int64_t{0}); // dedup absorbs the rerun
}

TEST(schema_is_configuration_not_code) {
    const dd::schema::Registry licenses = dd::schema::Registry::from_json(R"({
      "fields": [
        {"name": "license_number", "kind": "id", "identity": true,
         "synonyms": ["license", "license no", "permit number"]},
        {"name": "holder", "kind": "name", "role": "owner",
         "synonyms": ["licensee", "business name", "holder"]},
        {"name": "premises", "kind": "address", "identity": true,
         "synonyms": ["premises address", "business address", "location"]},
        {"name": "annual_fee", "kind": "money", "role": "amount",
         "synonyms": ["fee", "annual fee", "fee paid"]},
        {"name": "expires", "kind": "date", "role": "event_date",
         "synonyms": ["expiry", "expiration date", "valid until"]}
      ]
    })");
    const dd::doc::Model m = dd::doc::build_auto(
        "text/csv",
        "License No,Licensee,Business Address,Fee Paid,Expiration Date\n"
        "BL-2026-118,Harbor Coffee LLC,12 Pier St,250.00,2027-01-31\n"
        "BL-2026-204,Vega Auto Repair,900 Foundry Rd,410.00,2027-03-15\n");
    const dd::schema::Mapping mapping = dd::schema::infer_mapping(licenses, m);
    CHECK_EQ(mapping.fields.size(), std::size_t{5});
    CHECK_EQ(mapping.find("license_number")->source_label, "License No");
    CHECK_EQ(mapping.find("holder")->source_label, "Licensee");
    CHECK_EQ(mapping.find("premises")->source_label, "Business Address");
    CHECK_EQ(mapping.find("annual_fee")->source_label, "Fee Paid");
    CHECK_EQ(mapping.find("expires")->source_label, "Expiration Date");

    const dd::schema::ExtractionResult result =
        dd::schema::apply_mapping(licenses, mapping, m);
    CHECK_NEAR(result.rate, 1.0, 1e-9);
    CHECK_EQ(result.records[0].values.at("annual_fee"), "250.00");
    CHECK_EQ(result.records[1].values.at("expires"), "2027-03-15");
}

TEST(schema_coerces_embedded_values) {
    const dd::schema::FieldDef& parcel = *test_registry().find("parcel_id");
    const dd::schema::FieldDef& money = *test_registry().find("amount_due");
    const dd::schema::FieldDef& date = *test_registry().find("event_date");
    const dd::schema::FieldDef& owner = *test_registry().find("owner");

    const dd::schema::Coercion direct = dd::schema::coerce(parcel, "123-456-789");
    CHECK(direct.ok);
    CHECK(!direct.reformatted);
    CHECK_EQ(direct.value, "123-456-789");

    const dd::schema::Coercion acct = dd::schema::coerce(parcel, "Account: 123-456");
    CHECK(acct.ok);
    CHECK(acct.reformatted);
    CHECK_EQ(acct.value, "123-456");

    const dd::schema::Coercion due = dd::schema::coerce(money, "$1,204.77 past due");
    CHECK(due.ok);
    CHECK(due.reformatted);
    CHECK_EQ(due.value, "1204.77");

    const dd::schema::Coercion filed = dd::schema::coerce(date, "Filed 07/28/2026 by clerk");
    CHECK(filed.ok);
    CHECK(filed.reformatted);
    CHECK_EQ(filed.value, "2026-07-28");

    CHECK(!dd::schema::coerce(owner, "c/o agent for JANE SMITH et al 12345678901234567890"
                                     "12345678901234567890123456789012345678901234567890").ok);
    CHECK(!dd::schema::coerce(parcel, "no digits here at all").ok);
}

TEST(schema_maps_composite_columns_through_coercion) {
    const dd::doc::Model m = dd::doc::build_auto(
        "text/csv",
        "Parcel Number,Owner Name,Amount Due\n"
        "Account No: 111-22-001,\"Smith, Jane\",$100.00 due\n"
        "Account No: 111-22-002,\"Ray, Bob\",$250.50 due\n"
        "Account No: 111-22-003,\"Lee, Ann\",$75.25 due\n");
    const dd::schema::Mapping mapping = dd::schema::infer_mapping(test_registry(), m);
    const dd::schema::FieldMapping* parcel = mapping.find("parcel_id");
    CHECK(parcel != nullptr);
    CHECK(parcel->reformatted);
    CHECK_NEAR(parcel->value_pass_rate, 1.0, 1e-9);
    const dd::schema::FieldMapping* amount = mapping.find("amount_due");
    CHECK(amount != nullptr);
    CHECK(amount->reformatted);

    const dd::schema::ExtractionResult result =
        dd::schema::apply_mapping(test_registry(), mapping, m);
    CHECK_EQ(result.records[0].values.at("parcel_id"), "111-22-001");
    CHECK_EQ(result.records[1].values.at("amount_due"), "250.50");
    CHECK_NEAR(result.rate, 1.0, 1e-9);

    const dd::schema::Mapping loaded = dd::schema::Mapping::deserialize(mapping.serialize());
    CHECK(loaded.find("parcel_id")->reformatted);
}

TEST(schema_candidates_surface_near_misses_for_review) {
    const dd::doc::Model m = dd::doc::build_auto(
        "application/json",
        R"([{"address": "12 Pier St", "street_name": "OGDEN", "violation_date": "2026-01-05"},
            {"address": "9 Dock Rd", "street_name": "CANAL", "violation_date": "2026-01-06"},
            {"address": "4 Quay Ln", "street_name": "WHARF", "violation_date": "2026-01-07"}])");
    const std::vector<dd::schema::Candidate> candidates =
        dd::schema::score_candidates(test_registry(), m, 0.45);

    const dd::schema::Candidate* street_as_owner = nullptr;
    const dd::schema::Candidate* address_direct = nullptr;
    for (const dd::schema::Candidate& c : candidates) {
        if (c.field == "owner" && c.source_label == "street_name") street_as_owner = &c;
        if (c.field == "address" && c.source_label == "address") address_direct = &c;
    }
    CHECK(address_direct != nullptr);
    CHECK(address_direct->accepted);
    CHECK(street_as_owner != nullptr);
    CHECK(!street_as_owner->accepted); // held back by the weak-validator floor
    CHECK(street_as_owner->confidence >= 0.45);
}

TEST(schema_neural_evidence_maps_headers_the_lexicon_cannot) {
    const dd::schema::Registry registry = dd::schema::Registry::from_json(R"({"fields": [
        {"name": "parcel_id", "kind": "id", "identity": true,
         "synonyms": ["parcel", "apn"]},
        {"name": "owner", "kind": "name", "synonyms": ["owner", "taxpayer"]}
    ]})");

    std::mt19937 rng{23};
    std::vector<dd::columns::Example> corpus;
    static const char* kIdNames[] = {"tms", "sbl", "geo_ref", "prop_key", "roll_no"};
    static const char* kOwnerNames[] = {"party", "holder", "resident", "citizen", "deeded_to"};
    static const char* kPeople[] = {"maria lopez", "james hill", "ada okafor", "chen wei"};
    for (int i = 0; i < 60; ++i) {
        dd::columns::Example id_col;
        id_col.name = kIdNames[rng() % 5];
        for (int v = 0; v < 3; ++v) {
            id_col.values.push_back(std::to_string(100000 + static_cast<int>(rng() % 900000)));
        }
        id_col.label = "parcel_id";
        id_col.domain = "d" + std::to_string(i % 6);
        corpus.push_back(id_col);

        dd::columns::Example owner_col;
        owner_col.name = kOwnerNames[rng() % 5];
        for (int v = 0; v < 3; ++v) owner_col.values.push_back(kPeople[rng() % 4]);
        owner_col.label = "owner";
        owner_col.domain = "d" + std::to_string(i % 6);
        corpus.push_back(owner_col);
    }
    dd::columns::ColumnModel nn{tiny_hyper()};
    dd::columns::TrainConfig config;
    config.epochs = 15;
    config.batch = 16;
    config.threads = 2;
    nn.train(corpus, {}, config);

    const dd::doc::Model m = dd::doc::build_auto(
        "application/json",
        R"([{"gpin_key": "482113", "deeded_to": "maria lopez"},
            {"gpin_key": "903427", "deeded_to": "james hill"},
            {"gpin_key": "118755", "deeded_to": "chen wei"}])");

    const dd::schema::Mapping without = dd::schema::infer_mapping(registry, m);
    CHECK(without.find("parcel_id") == nullptr);

    const dd::schema::Mapping with = dd::schema::infer_mapping(registry, m, &nn);
    const dd::schema::FieldMapping* mapped = with.find("parcel_id");
    CHECK(mapped != nullptr);
    CHECK_EQ(mapped->source_label, "gpin_key");
}

TEST(schema_mapping_deserialize_rejects_duplicates_and_bad_scores) {
    CHECK_THROWS(dd::schema::Mapping::deserialize(R"({"fields": [
        {"field": "parcel_id", "source_label": "a", "confidence": 0.9},
        {"field": "parcel_id", "source_label": "b", "confidence": 0.9}]})"));
    CHECK_THROWS(dd::schema::Mapping::deserialize(R"({"fields": [
        {"field": "parcel_id", "source_label": "a", "confidence": 0.9},
        {"field": "owner", "source_label": "a", "confidence": 0.9}]})"));
    CHECK_THROWS(dd::schema::Mapping::deserialize(R"({"fields": [
        {"field": "parcel_id", "source_label": "a", "confidence": 1.5}]})"));
}

TEST(harvest_weak_labels_mask_near_misses_and_conflicts) {
    const dd::schema::Registry& registry = test_registry();
    const dd::harvest::WeakLabel exact = dd::harvest::weak_label(registry, "apn", "APN");
    CHECK_EQ(exact.field, "parcel_id");
    CHECK(!exact.masked);
    const dd::harvest::WeakLabel near = dd::harvest::weak_label(registry, "owner_nm", "");
    CHECK(near.field.empty());
    CHECK(near.masked);
    const dd::harvest::WeakLabel conflict =
        dd::harvest::weak_label(registry, "apn", "Case Number");
    CHECK(conflict.field.empty());
    CHECK(conflict.masked);
    const dd::harvest::WeakLabel none =
        dd::harvest::weak_label(registry, "wind_speed_mph", "Wind Speed");
    CHECK(none.field.empty());
    CHECK(!none.masked);
}

TEST(entity_address_parts_join_across_office_dialects) {
    using dd::entity::parse_address;
    const dd::entity::Address treasurer = parse_address("0555 LIBERTY ST E");
    const dd::entity::Address assessor = parse_address("555 Liberty ST");
    CHECK_EQ(treasurer.number, "555");
    CHECK_EQ(treasurer.street, "liberty");
    CHECK_EQ(treasurer.directional, "e");
    CHECK_EQ(assessor.directional, "");
    CHECK_EQ(dd::entity::address_join_key(treasurer), dd::entity::address_join_key(assessor));
    CHECK(dd::entity::compatible(treasurer, assessor));

    CHECK(!dd::entity::compatible(parse_address("100 MAIN ST E"), parse_address("100 MAIN ST W")));
    CHECK(!dd::entity::compatible(parse_address("100 MAIN ST"), parse_address("100 MAIN AVE")));

    CHECK(dd::entity::address_join_key(parse_address("3 COMMERCIAL PL A")) !=
          dd::entity::address_join_key(parse_address("3 COMMERCIAL PL C")));

    CHECK(!parse_address("3200 BLOCK OF ARGONNE AVENUE").locatable);
    CHECK(!parse_address("0 ADMIRAL TAUSSIG BLVD").locatable);
    CHECK(!parse_address("S S 50TH ST").locatable);
    CHECK(parse_address("9628 10TH BAY ST").locatable);
}

TEST(entity_addresses_merge_across_padding_and_suffix_dialects) {
    using dd::entity::normalize_address;
    CHECK_EQ(normalize_address("436 W 31ST ST"), normalize_address("436 W 31st STREET"));
    CHECK_EQ(normalize_address("0555 LIBERTY ST E"), normalize_address("555 Liberty Street E"));
    CHECK_EQ(normalize_address("3318 E OCEAN VIEW AV"),
             normalize_address("3318 East Ocean View AVENUE"));
    CHECK(normalize_address("100 MAIN ST") != normalize_address("102 MAIN ST"));
}

TEST(compile_merges_id_spaces_and_resolves_conflicts_by_trust) {
    const std::string root = fresh_dir("compile_county");
    dd::store::Store store{root};
    store.add_source("https://a.example/tax", "Treasurer", "Testville VA");
    store.add_source("https://b.example/assess", "Assessor", "Testville VA");
    const std::vector<dd::store::Source> sources = store.sources();

    auto save_state = [&](const std::string& source_id, double owner_confidence) {
        dd::store::SourceState state = store.source_state(source_id);
        state.has_mapping = true;
        state.mapping.fields.push_back(dd::schema::FieldMapping{
            "owner", "who", 0.9, 0.9, owner_confidence, false});
        store.save_source_state(state);
    };
    save_state(sources[0].id, 0.62);
    save_state(sources[1].id, 0.97);

    auto make = [&](const std::string& source_id, const std::string& parcel,
                    const std::string& address, const std::string& owner,
                    dd::events::Kind kind, double amount, const std::string& date) {
        dd::events::PropertyEvent e;
        e.property_key = dd::entity::property_key("Testville VA", parcel, address);
        e.kind = kind;
        e.event_date = date;
        e.recorded_at = date + "T00:00:00Z";
        e.source_id = source_id;
        e.amount = amount;
        e.confidence = 0.9;
        e.details["address"] = address;
        e.details["owner"] = owner;
        if (kind == dd::events::Kind::AssessmentRecorded) {
            e.details["assessed_value"] = "500000";
        }
        e.id = dd::events::PropertyEvent::compute_id(e);
        return e;
    };
    store.add_events({
        make(sources[0].id, "111", "0436 W 31ST ST", "PARK PLACE DEV LLC",
             dd::events::Kind::TaxDelinquency, 110091.0, "2026-07-01"),
        make(sources[1].id, "999", "436 W 31st STREET", "Parker Plaza Holdings",
             dd::events::Kind::AssessmentRecorded, 0.0, "2026-06-01"),
        make(sources[1].id, "555", "", "No Address LLC",
             dd::events::Kind::AssessmentRecorded, 0.0, "2026-06-01"),
    });

    const dd::schema::Registry registry = test_registry();
    const std::vector<dd::compile::Property> properties =
        dd::compile::county(store, registry, "testville");
    CHECK_EQ(properties.size(), std::size_t{2}); // merged pair + the address-less one

    const dd::compile::Property& merged = properties.front(); // owed sorts first
    CHECK_EQ(merged.keys.size(), std::size_t{2});
    CHECK_NEAR(merged.due, 110091.0, 1e-9);
    CHECK_NEAR(merged.assessed, 500000.0, 1e-9);
    CHECK_EQ(merged.fields.at("owner").value, "Parker Plaza Holdings");
    CHECK_NEAR(merged.fields.at("owner").confidence, 0.97, 1e-9);
    CHECK_EQ(merged.conflicts.size(), std::size_t{1});
    CHECK_EQ(merged.conflicts[0].field, "owner");
    CHECK_EQ(merged.conflicts[0].kept_source, sources[1].id);
    CHECK_EQ(merged.conflicts[0].dropped_value, "PARK PLACE DEV LLC");

    store.add_events({
        make(sources[1].id, "701", "0 NAVY BASE RD", "US Navy",
             dd::events::Kind::AssessmentRecorded, 0.0, "2026-06-02"),
        make(sources[1].id, "702", "0 NAVY BASE RD", "US Navy",
             dd::events::Kind::AssessmentRecorded, 0.0, "2026-06-02"),
    });
    const std::vector<dd::compile::Property> again =
        dd::compile::county(store, registry, "testville");
    CHECK_EQ(again.size(), std::size_t{4}); // merged pair + no-address + two placeholders
}

TEST(html_query_dfs_and_selectors_compose) {
    const dd::html::Document doc = dd::html::parse(R"(
        <html><body>
          <table class="data results"><tr><th>Parcel</th><td>123</td></tr></table>
          <div class="results"><span>Owed: $500</span></div>
          <table id="nav"><tr><td>menu</td></tr></table>
        </body></html>)");

    using namespace dd::html;
    const std::size_t tables = static_cast<std::size_t>(std::count_if(
        dfs(doc).begin(), dfs(doc).end(), [](const Node& n) {
            return n.kind == Node::Kind::Element && n.tag == "table";
        }));
    CHECK_EQ(tables, std::size_t{2});

    CHECK_EQ(select(doc, tag("table") && has_class("data")).size(), std::size_t{1});
    CHECK_EQ(select(doc, tag("table") && !has_class("data")).size(), std::size_t{1});
    CHECK_EQ(select(doc, has_class("results")).size(), std::size_t{2});
    CHECK_EQ(select(doc, tag("span") && text_contains("owed")).size(), std::size_t{1});
    CHECK_EQ(select(doc, attr_equals("id", "nav")).size(), std::size_t{1});
    const Node* first = select_first(doc.root(), tag("th"));
    CHECK(first != nullptr);
    CHECK_EQ(first->text_content(), "Parcel");
    CHECK(select_first(doc.root(), tag("video")) == nullptr);
}

TEST(fetch_render_scheme_uses_external_renderer) {
    setenv("DD_RENDERER", "cat data/fixtures/millbrook_tax.html #", 1);
    const dd::fetch::Result rendered = dd::fetch::get("render+https://spa.example/parcels");
    CHECK(rendered.ok);
    CHECK_EQ(rendered.content_type, "text/html");
    CHECK(rendered.bytes > 0);
    CHECK(dd::str::contains(rendered.body, "<table"));

    unsetenv("DD_RENDERER");
    CHECK(!dd::fetch::get("render+https://spa.example/parcels").ok);
    setenv("DD_RENDERER", "false", 1);
    CHECK(!dd::fetch::get("render+https://spa.example/parcels").ok);
    CHECK(!dd::fetch::get("render+ftp://nope").ok);
    CHECK(!dd::fetch::get("render+https://x.test/'; rm -rf ~'").ok);
    unsetenv("DD_RENDERER");
}

TEST(pipeline_runs_sources_concurrently_without_losing_records) {
    const std::string root = fresh_dir("pipe_parallel");
    dd::store::Store store{root};
    std::vector<dd::store::Source> sources;
    for (int i = 0; i < 6; ++i) {
        sources.push_back(store.add_source("fixture " + std::to_string(i),
                                           "data/fixtures/millbrook_tax.html", "Millbrook NY"));
    }
    dd::pipeline::Pipeline pipeline{store, test_classifier(), test_registry()};
    const std::vector<dd::store::RunRecord> runs = pipeline.run_sources(sources, 4);

    CHECK_EQ(runs.size(), sources.size());
    for (std::size_t i = 0; i < runs.size(); ++i) {
        CHECK(runs[i].ok);
        CHECK_EQ(runs[i].source_id, sources[i].id); // order preserved
        CHECK(runs[i].records_extracted > 0);
    }
    CHECK_EQ(store.runs(100).size(), sources.size());
    for (const dd::store::Source& s : sources) {
        CHECK(store.source_state(s.id).has_mapping);
    }
}

TEST(core_hours_between_reads_dates_and_stamps) {
    CHECK_NEAR(dd::timeutil::hours_between("2026-07-27T00:00:00Z", "2026-07-29T00:00:00Z"),
               48.0, 1e-6);
    CHECK_NEAR(dd::timeutil::hours_between("2026-07-27", "2026-07-27T06:00:00Z"), 6.0, 1e-6);
    CHECK(dd::timeutil::hours_between("2026-08-11", "2026-07-29") < 0.0);  // scheduled ahead
    CHECK(dd::timeutil::hours_between("not a date", "2026-07-29") < 0.0);
}

TEST(compile_prefers_the_current_edition_and_keeps_the_previous) {
    const std::string root = fresh_dir("compile_vintage");
    dd::store::Store store{root};
    dd::store::Source current = store.add_source("Roll FY25", "https://a/25", "Testville VA");
    dd::store::Source prior = store.add_source("Roll FY24", "https://a/24", "Testville VA");

    auto assessment = [&](const std::string& source_id, const std::string& as_of,
                          const std::string& value, const std::string& date) {
        dd::events::PropertyEvent e;
        e.property_key = dd::entity::property_key("Testville VA", "P1", "10 Oak ST");
        e.kind = dd::events::Kind::AssessmentRecorded;
        e.event_date = date;
        e.recorded_at = date + "T00:00:00Z";
        e.source_id = source_id;
        e.as_of = as_of;
        e.confidence = 0.9;
        e.details["address"] = "10 Oak ST";
        e.details["assessed_value"] = value;
        e.id = dd::events::PropertyEvent::compute_id(e);
        return e;
    };
    store.add_events({assessment(current.id, "2025", "615400", "2020-01-01"),
                      assessment(prior.id, "2024", "611700", "2024-06-01")});

    const std::vector<dd::compile::Property> properties =
        dd::compile::county(store, test_registry(), "testville");
    CHECK_EQ(properties.size(), std::size_t{1});
    const dd::compile::Property& p = properties.front();
    CHECK_EQ(p.fields.at("assessed_value").value, "615400");
    CHECK_EQ(p.fields.at("assessed_value").as_of, "2025");
    CHECK_NEAR(p.assessed, 615400.0, 1e-9);
    CHECK_NEAR(p.assessed_previous, 611700.0, 1e-9);
    CHECK_EQ(p.history.at("assessed_value").size(), std::size_t{2});
    CHECK_EQ(p.history.at("assessed_value").front().as_of, "2025");
}

TEST(compile_measures_days_since_the_newest_event) {
    const std::string root = fresh_dir("compile_recency");
    dd::store::Store store{root};
    const dd::store::Source source = store.add_source("Roll", "https://a/roll", "Testville VA");
    const std::int64_t now = dd::timeutil::unix_now();
    const auto days_ago = [&](int days) {
        return dd::timeutil::iso_from_unix(now - static_cast<std::int64_t>(days) * 86400);
    };

    auto event = [&](const std::string& parcel, const std::string& address,
                     const std::string& date) {
        dd::events::PropertyEvent e;
        e.property_key = dd::entity::property_key("Testville VA", parcel, address);
        e.kind = dd::events::Kind::AssessmentRecorded;
        e.event_date = date;
        e.recorded_at = dd::timeutil::iso_now();
        e.source_id = source.id;
        e.confidence = 0.9;
        e.details["address"] = address;
        e.details["parcel_id"] = parcel;
        e.id = dd::events::PropertyEvent::compute_id(e);
        return e;
    };
    store.add_events({
        event("F1", "10 FRESH ST", days_ago(45)),
        event("F1", "10 FRESH ST", days_ago(3)),
        event("S1", "20 STALE ST", days_ago(400)),
        event("U1", "30 UNDATED ST", ""),
        event("U1", "30 UNDATED ST", "call the clerk"),
        event("A1", "40 ANCIENT ST", "1850-01-01"),
    });

    const std::vector<dd::compile::Property> properties =
        dd::compile::county(store, test_registry(), "testville");
    std::map<std::string, std::optional<std::int64_t>> measured;
    for (const dd::compile::Property& p : properties) {
        const auto parcel = p.fields.find("parcel_id");
        if (parcel == p.fields.end()) continue;
        measured[parcel->second.value] = p.days_since_event;
    }
    CHECK_EQ(measured.size(), std::size_t{4});
    CHECK(measured["F1"] == std::optional<std::int64_t>{3}); // the newest event, not the oldest
    CHECK(measured["S1"] == std::optional<std::int64_t>{400});
    CHECK(!measured["U1"].has_value());
    CHECK(!measured["A1"].has_value()); // 1850 predates any instant this engine can date

    const dd::json::Value rendered =
        dd::json::parse(dd::compile::render_county_json("Testville VA", properties));
    std::map<std::string, std::string> emitted;
    for (const dd::json::Value& record : rendered.find("records")->items()) {
        const dd::json::Value* parcel = record.find("fields")->find("parcel_id");
        const dd::json::Value* days = record.find("signals")->find("days_since_event");
        emitted[parcel->find("value")->as_string()] = days == nullptr ? "" : days->serialize();
    }
    CHECK_EQ(emitted.size(), std::size_t{4});
    CHECK_EQ(emitted["F1"], "3");
    CHECK_EQ(emitted["S1"], "400");
    CHECK_EQ(emitted["U1"], ""); // absent, not zero: an unmeasured lead never reads as live
    CHECK_EQ(emitted["A1"], "");
}

TEST(schema_registry_rejects_bad_files) {
    CHECK_THROWS(dd::schema::Registry::from_json("{}"));
    CHECK_THROWS(dd::schema::Registry::from_json(R"({"fields": []})"));
    CHECK_THROWS(dd::schema::Registry::from_json(
        R"({"fields": [{"name": "a", "kind": "nope", "identity": true}]})"));
    CHECK_THROWS(dd::schema::Registry::from_json(
        R"({"fields": [{"name": "a", "kind": "id", "identity": true},
                       {"name": "a", "kind": "id"}]})"));
    CHECK_THROWS(dd::schema::Registry::from_json(
        R"({"fields": [{"name": "amount", "kind": "money"}]})"));
    CHECK_THROWS(dd::schema::Registry::load("data/no_such_schema.json"));
}

TEST(document_survives_bespoke_legacy_markup) {
    const std::string body = dd::fileio::read_file("data/fixtures/millbrook_code_enforcement.html");
    const dd::doc::Model m = dd::doc::build_auto("text/html", body);
    CHECK_EQ(m.records.size(), std::size_t{3});
    const dd::doc::Cell* parcel = m.records[0].find("Parcel");
    CHECK(parcel != nullptr);
    CHECK_EQ(parcel->value, "04-118-002");
    const dd::doc::Cell* case_no = m.records[0].find("Case No");
    CHECK(case_no != nullptr);
    CHECK_EQ(case_no->value, "CE-26-0771");
}

TEST(pipeline_ingests_real_government_source) {
    const std::string root = fresh_dir("pipe_real");
    dd::store::Store store{root};
    dd::pipeline::Pipeline pipeline{store, test_classifier(), test_registry()};
    const dd::store::Source source = store.add_source(
        "Norfolk VA Delinquent Taxes",
        "https://data.norfolk.gov/resource/7qie-z5gv.json?$select=account,owner_name,address,"
        "sum(taxdue) as tax_due,sum(total) as total_due&$group=account,owner_name,address"
        "&$order=total_due DESC&$limit=25",
        "City of Norfolk VA");
    const dd::store::RunRecord run = pipeline.run_source(source);
    if (!run.ok && run.stage == "fetch") {
        std::printf("     (network unavailable, real-source ingestion skipped: %s)\n",
                    run.error.c_str());
        return;
    }
    CHECK(run.ok);
    CHECK_EQ(run.classification, "tax_delinquency");
    CHECK_EQ(run.records_extracted, std::int64_t{25});
    CHECK(run.extraction_rate > 0.8);
    CHECK(run.events_new > 0);
}
} // namespace

int main() {
    int ran = 0;
    for (const TestCase& test : registry()) {
        g_current = test.name.c_str();
        const int before = g_failures;
        try {
            test.body();
        } catch (const std::exception& e) {
            report(__FILE__, 0, std::string{"unhandled exception: "} + e.what());
        }
        ++ran;
        if (g_failures == before) std::printf("ok   %s\n", test.name.c_str());
    }
    std::printf("%d tests, %d failures\n", ran, g_failures);
    return g_failures == 0 ? 0 : 1;
}
