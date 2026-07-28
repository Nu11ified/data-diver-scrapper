// Behaviour tests for the Data Diver engine. Each test exercises a module
// through its public interface only; ctest runs this binary from the source
// tree so fixtures resolve by relative path.

#include "dd/ml/classify.hpp"
#include "dd/core/core.hpp"
#include "dd/parse/csv.hpp"
#include "dd/parse/document.hpp"
#include "dd/engine/entity.hpp"
#include "dd/engine/events.hpp"
#include "dd/ml/features.hpp"
#include "dd/net/fetch.hpp"
#include "dd/parse/html.hpp"
#include "dd/core/json.hpp"
#include "dd/engine/heal.hpp"
#include "dd/core/metrics.hpp"
#include "dd/ml/model.hpp"
#include "dd/parse/pdf.hpp"
#include "dd/engine/pipeline.hpp"
#include "dd/engine/schema.hpp"
#include "dd/net/server.hpp"
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

// ---------------------------------------------------------------- core -----

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

// ---------------------------------------------------------------- json -----

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

// ---------------------------------------------------------------- html -----

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

// ----------------------------------------------------------------- csv -----

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

// ------------------------------------------------------------- metrics -----

TEST(metrics_report_real_values) {
    CHECK(dd::metrics::current_rss_bytes() > 1024 * 1024);
    CHECK(dd::metrics::peak_rss_bytes() >= dd::metrics::current_rss_bytes() / 2);
    CHECK(dd::metrics::cpu_time_ms() > 0.0);
}

// --------------------------------------------------------------- fetch -----

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

// ----------------------------------------------------------------- pdf -----

namespace pdfgen {

// Builds a minimal one-page PDF whose content stream is `content`,
// optionally Flate compressed. Real header, real objects, real layout.
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
    // The -500 kern must render as a column gap, not the number itself.
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

// ------------------------------------------------------------- document ----

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
    // Malformed JSON falls through to text rather than claiming success.
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

// ------------------------------------------------------------ features -----

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

// --------------------------------------------------------------- model -----

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

TEST(model_rejects_bad_serialization) {
    CHECK_THROWS(dd::model::NaiveBayes::deserialize("{\"kind\":\"other\"}"));
    CHECK_THROWS(dd::model::NaiveBayes::deserialize(
        "{\"kind\":\"naive_bayes_multinomial\",\"classes\":[]}"));
}

// ------------------------------------------------------------ classify -----

TEST(classifier_trains_with_high_holdout_accuracy) {
    dd::classify::TrainReport train_report;
    const dd::classify::Classifier classifier =
        dd::classify::Classifier::train_from_corpus("data/corpus", &train_report);
    CHECK_EQ(train_report.classes, std::size_t{8});
    CHECK(train_report.examples >= 40);
    CHECK(train_report.leave_one_out_accuracy >= 0.85);

    // A document the corpus has never seen, in tax-delinquency dialect.
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

    // And a probate docket must not classify as tax delinquency.
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
    CHECK_NEAR(loaded.trained_accuracy(), trained.trained_accuracy(), 1e-12);
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

// -------------------------------------------------------------- schema -----

TEST(schema_money_parsing) {
    CHECK_NEAR(*dd::schema::parse_money("$8,421.37"), 8421.37, 1e-9);
    CHECK_NEAR(*dd::schema::parse_money("120"), 120.0, 1e-9);
    CHECK_NEAR(*dd::schema::parse_money("$ 1,000"), 1000.0, 1e-9);
    CHECK(!dd::schema::parse_money("Jane Smith").has_value());
    CHECK(!dd::schema::parse_money("12-34").has_value());
    CHECK(!dd::schema::parse_money("").has_value());
}

TEST(schema_date_parsing) {
    CHECK_EQ(*dd::schema::parse_date("2026-06-18"), "2026-06-18");
    CHECK_EQ(*dd::schema::parse_date("06/18/2026"), "2026-06-18");
    CHECK_EQ(*dd::schema::parse_date("June 18, 2026"), "2026-06-18");
    CHECK_EQ(*dd::schema::parse_date("18 June 2026"), "2026-06-18");
    CHECK(!dd::schema::parse_date("Jane Smith").has_value());
    CHECK(!dd::schema::parse_date("123-456-789").has_value());
    CHECK(!dd::schema::parse_date("99/99/2026").has_value());
}

TEST(schema_validators) {
    using dd::schema::Field;
    CHECK(dd::schema::validate(Field::ParcelId, "123-456-789"));
    CHECK(dd::schema::validate(Field::ParcelId, "201-33-0870"));
    CHECK(!dd::schema::validate(Field::ParcelId, "Jane Smith"));
    CHECK(!dd::schema::validate(Field::ParcelId, "2026-06-18")); // a date is not a parcel
    CHECK(!dd::schema::validate(Field::ParcelId, "$1,200.00"));

    CHECK(dd::schema::validate(Field::Owner, "Smith, Jane"));
    CHECK(!dd::schema::validate(Field::Owner, "123-456"));

    CHECK(dd::schema::validate(Field::Address, "1402 Main Street"));
    CHECK(dd::schema::validate(Field::Address, "PO Box 118"));
    CHECK(!dd::schema::validate(Field::Address, "8421.37"));

    CHECK(dd::schema::validate(Field::AmountDue, "$8,421.37"));
    CHECK(!dd::schema::validate(Field::AmountDue, "Jane"));

    CHECK(dd::schema::validate(Field::EventDate, "2026-06-18"));
    CHECK(!dd::schema::validate(Field::EventDate, "next week"));

    CHECK(dd::schema::validate(Field::CaseNumber, "2026-CV-04182"));
    CHECK(!dd::schema::validate(Field::CaseNumber, "$500"));
}

TEST(schema_normalization) {
    using dd::schema::Field;
    CHECK_EQ(dd::schema::normalize(Field::AmountDue, "$8,421.37"), "8421.37");
    CHECK_EQ(dd::schema::normalize(Field::EventDate, "06/18/2026"), "2026-06-18");
    CHECK_EQ(dd::schema::normalize(Field::ParcelId, "12a-33"), "12A-33");
    CHECK_EQ(dd::schema::normalize(Field::Owner, "  Jane   Smith "), "Jane Smith");
}

TEST(schema_infers_mapping_across_dialects) {
    // Dialect one: conventional column names.
    const dd::doc::Model a = dd::doc::build_auto(
        "text/csv",
        "Parcel Number,Owner Name,Property Address,Amount Due,Sale Date\n"
        "111-22-333,\"Smith, Jane\",19 Birch Ln,\"$2,114.90\",2026-08-01\n"
        "444-55-666,\"Ray, Bob\",820 Canal Rd,$860.02,2026-08-01\n");
    const dd::schema::Mapping ma = dd::schema::infer_mapping(a);
    CHECK(ma.find(dd::schema::Field::ParcelId) != nullptr);
    CHECK_EQ(ma.find(dd::schema::Field::ParcelId)->source_label, "Parcel Number");
    CHECK(ma.find(dd::schema::Field::Owner) != nullptr);
    CHECK(ma.find(dd::schema::Field::Address) != nullptr);
    CHECK(ma.find(dd::schema::Field::AmountDue) != nullptr);
    CHECK(ma.find(dd::schema::Field::AuctionDate) != nullptr);
    CHECK(ma.confidence > 0.7);

    // Dialect two: a different county's vocabulary for the same facts.
    const dd::doc::Model b = dd::doc::build_auto(
        "application/json",
        R"({"rows": [
            {"apn": "77-100-08", "taxpayer": "Nguyen, An", "situs": "12 Fern Way", "balance": 902.11},
            {"apn": "77-100-31", "taxpayer": "Cole, Dana", "situs": "77 Mill St", "balance": 5210.40}
        ]})");
    const dd::schema::Mapping mb = dd::schema::infer_mapping(b);
    CHECK(mb.find(dd::schema::Field::ParcelId) != nullptr);
    CHECK_EQ(mb.find(dd::schema::Field::ParcelId)->source_label, "apn");
    CHECK(mb.find(dd::schema::Field::Owner) != nullptr);
    CHECK_EQ(mb.find(dd::schema::Field::Owner)->source_label, "taxpayer");
    CHECK(mb.find(dd::schema::Field::Address) != nullptr);
    CHECK(mb.find(dd::schema::Field::AmountDue) != nullptr);
    CHECK_EQ(mb.find(dd::schema::Field::AmountDue)->source_label, "balance");
}

TEST(schema_mapping_requires_identity_field) {
    const dd::doc::Model m = dd::doc::build_auto(
        "text/csv", "Amount,Date\n$100,2026-01-01\n$200,2026-01-02\n$300,2026-01-03\n");
    const dd::schema::Mapping mapping = dd::schema::infer_mapping(m);
    CHECK(mapping.fields.empty());
    CHECK_NEAR(mapping.confidence, 0.0, 1e-12);
}

TEST(schema_mapping_confidence_reflects_measurements) {
    const dd::doc::Model m = dd::doc::build_auto(
        "text/csv",
        "Parcel,Owner\n111-22,Jane\n444-55,Bob\n777-88,Sue\n");
    const dd::schema::Mapping mapping = dd::schema::infer_mapping(m);
    const dd::schema::FieldMapping* parcel = mapping.find(dd::schema::Field::ParcelId);
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
    const dd::schema::Mapping mapping = dd::schema::infer_mapping(m);
    const dd::schema::ExtractionResult result = dd::schema::apply_mapping(mapping, m);
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
    const dd::schema::Mapping mapping = dd::schema::infer_mapping(m);
    const dd::schema::Mapping loaded = dd::schema::Mapping::deserialize(mapping.serialize());
    CHECK_EQ(loaded.fields.size(), mapping.fields.size());
    CHECK_NEAR(loaded.confidence, mapping.confidence, 1e-12);
    const dd::schema::FieldMapping* owner = loaded.find(dd::schema::Field::Owner);
    CHECK(owner != nullptr);
    CHECK_EQ(owner->source_label, "Owner");
}

// -------------------------------------------------------------- entity -----

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
    // Same parcel in a different jurisdiction is a different property.
    CHECK(dd::entity::property_key("A", "123", "") != dd::entity::property_key("B", "123", ""));
}

TEST(entity_same_owner) {
    CHECK(dd::entity::same_owner("Smith, Jane", "Jane Smith"));
    CHECK(dd::entity::same_owner("SMITH JANE", "jane smith"));
    CHECK(!dd::entity::same_owner("Jane Smith", "Bob Ray"));
    CHECK(!dd::entity::same_owner("", "Bob Ray"));
}

// -------------------------------------------------------------- events -----

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
    // Deliberately shuffled input: the reducer must order by date itself.
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

// --------------------------------------------------------------- store -----

std::string fresh_dir(const std::string& name) {
    const std::string dir = "build/test_tmp/" + name;
    std::filesystem::remove_all(dir);
    dd::fileio::ensure_dir(dir);
    return dir;
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
    // Seeding twice must not duplicate.
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

// ------------------------------------------------------------ pipeline -----

dd::classify::Classifier test_classifier() {
    return dd::classify::Classifier::load("data/model/source_classifier.json");
}

TEST(pipeline_learns_and_ingests_table_site) {
    const std::string root = fresh_dir("pipe_learn");
    dd::store::Store store{root};
    const dd::store::Source source =
        store.add_source("Millbrook Tax", "data/fixtures/millbrook_tax.html", "Millbrook County");
    dd::pipeline::Pipeline pipeline{store, test_classifier()};

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

    // The learned state is persisted with a baseline.
    const dd::store::SourceState state = store.source_state(source.id);
    CHECK(state.has_mapping);
    CHECK_NEAR(state.baseline_rate, run.extraction_rate, 1e-9);
    CHECK_EQ(state.good_runs, 1);

    // Events resolve to jurisdiction-scoped properties.
    const std::vector<std::string> keys = store.property_keys();
    CHECK_EQ(keys.size(), std::size_t{6});
    CHECK(dd::str::contains(keys[0], "millbrook_county|p:"));

    // Re-ingesting the same bytes creates nothing new.
    const dd::store::RunRecord again = pipeline.run_source(source);
    CHECK(again.ok);
    CHECK_EQ(again.events_new, std::int64_t{0});
    CHECK(!again.drift_detected);
}

TEST(pipeline_handles_json_csv_pdf_dialects) {
    const std::string root = fresh_dir("pipe_dialects");
    dd::store::Store store{root};
    dd::pipeline::Pipeline pipeline{store, test_classifier()};

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

    // The sold-at-auction row must produce a SoldAtAuction event and drive
    // that property's lifecycle there.
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
    dd::pipeline::Pipeline pipeline{store, test_classifier()};
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

    // Day one: the county publishes a table.
    dd::fileio::write_file_atomic(site, dd::fileio::read_file("data/fixtures/millbrook_tax.html"));
    const dd::store::Source source = store.add_source("Millbrook Drift", site, "Millbrook County");
    dd::pipeline::Pipeline pipeline{store, test_classifier()};

    const dd::store::RunRecord first = pipeline.run_source(source);
    CHECK(first.ok);
    CHECK_EQ(first.events_new, std::int64_t{6});
    const std::string old_fingerprint = first.structure_fingerprint;

    // Day two: the county redesigns. Same facts, new markup, renamed labels,
    // one new delinquent parcel.
    dd::fileio::write_file_atomic(site,
                                  dd::fileio::read_file("data/fixtures/millbrook_tax_v2.html"));
    const dd::store::RunRecord second = pipeline.run_source(source);
    CHECK(second.ok);
    CHECK(second.drift_detected);
    CHECK(second.repair_attempted);
    CHECK(second.repair_accepted);
    CHECK(second.structure_fingerprint != old_fingerprint);
    CHECK(second.extraction_rate > 0.8);
    // Six parcels unchanged (dedup), one newly delinquent.
    CHECK_EQ(second.events_new, std::int64_t{1});

    // The repair is on the record with its evidence and its diff.
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

    // The healed mapping is now the accepted one and keeps working.
    const dd::store::RunRecord third = pipeline.run_source(source);
    CHECK(third.ok);
    CHECK(!third.drift_detected);
    CHECK_EQ(third.events_new, std::int64_t{0});
}

TEST(heal_assessment_ignores_healthy_updates) {
    // Content changed but extraction still works: not drift.
    const dd::doc::Model m = dd::doc::build_auto(
        "text/csv", "Parcel,Owner\n111-22,Jane\n444-55,Bob\n777-88,Sue\n");
    const dd::schema::Mapping mapping = dd::schema::infer_mapping(m);
    const dd::schema::ExtractionResult extraction = dd::schema::apply_mapping(mapping, m);

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

// -------------------------------------------------------------- server -----

struct HttpReply {
    int status = 0;
    std::string body;
};

// Minimal real HTTP client over a socket so the tests exercise the server's
// actual request parsing, including POST bodies.
HttpReply http_request(int port, const std::string& method, const std::string& target,
                       const std::string& body = "") {
    HttpReply reply;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return reply;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return reply;
    }
    std::string request = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    if (!body.empty()) {
        request += "Content-Type: application/json\r\nContent-Length: " +
                   std::to_string(body.size()) + "\r\n";
    }
    request += "Connection: close\r\n\r\n" + body;
    ::send(fd, request.data(), request.size(), 0);

    std::string raw;
    char buffer[8192];
    ssize_t n = 0;
    while ((n = ::recv(fd, buffer, sizeof(buffer), 0)) > 0) {
        raw.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(fd);

    const std::size_t space = raw.find(' ');
    if (space != std::string::npos) reply.status = std::atoi(raw.c_str() + space + 1);
    const std::size_t split = raw.find("\r\n\r\n");
    if (split != std::string::npos) reply.body = raw.substr(split + 4);
    return reply;
}

TEST(server_full_flow_over_real_http) {
    const std::string root = fresh_dir("server_flow");
    dd::store::Store store{root};
    dd::pipeline::Pipeline pipeline{store, test_classifier()};
    dd::server::Server server{store, pipeline, dd::server::Options{"127.0.0.1", 0, "web"}};
    server.start();
    CHECK(server.port() > 0);

    // The UI ships.
    const HttpReply ui = http_request(server.port(), "GET", "/");
    CHECK_EQ(ui.status, 200);
    CHECK(dd::str::contains(ui.body, "Data Diver"));

    // Empty overview.
    const HttpReply overview0 = http_request(server.port(), "GET", "/api/overview");
    CHECK_EQ(overview0.status, 200);
    const dd::json::Value parsed0 = dd::json::parse(overview0.body);
    CHECK_NEAR(parsed0.find("totals")->find("sources")->as_number(), 0.0, 1e-9);
    CHECK(parsed0.find("engine")->find("rss_bytes")->as_number() > 1e6);

    // Add a site through the API.
    const HttpReply added = http_request(
        server.port(), "POST", "/api/sources",
        R"({"name": "Crestline Auctions", "url": "data/fixtures/crestline_auctions.csv", "jurisdiction": "Crestline County"})");
    CHECK_EQ(added.status, 200);
    const std::string source_id = dd::json::parse(added.body).find("id")->as_string();
    CHECK(!source_id.empty());

    // Bad add is rejected.
    CHECK_EQ(http_request(server.port(), "POST", "/api/sources", R"({"name": "x"})").status, 400);

    // Run it.
    const HttpReply ran = http_request(server.port(), "POST", "/api/run?source=" + source_id);
    CHECK_EQ(ran.status, 200);
    const dd::json::Value run = dd::json::parse(ran.body);
    CHECK(run.find("ok")->as_bool());
    CHECK_EQ(run.find("classification")->as_string(), "trustee_auction");

    // Everything is visible through the API afterwards.
    const dd::json::Value runs =
        dd::json::parse(http_request(server.port(), "GET", "/api/runs?limit=5").body);
    CHECK_EQ(runs.items().size(), std::size_t{1});

    const dd::json::Value props =
        dd::json::parse(http_request(server.port(), "GET", "/api/properties").body);
    CHECK_EQ(props.items().size(), std::size_t{4});
    const std::string key = props.items()[0].find("key")->as_string();

    const HttpReply prop = http_request(
        server.port(), "GET", "/api/property?key=" + key);
    CHECK_EQ(prop.status, 200);
    CHECK(dd::json::parse(prop.body).find("events")->items().size() >= 1);

    const dd::json::Value records = dd::json::parse(
        http_request(server.port(), "GET", "/api/records?source=" + source_id).body);
    CHECK_EQ(records.items().size(), std::size_t{4});

    // Unknowns are 404s, not lies.
    CHECK_EQ(http_request(server.port(), "GET", "/api/nope").status, 404);
    CHECK_EQ(http_request(server.port(), "POST", "/api/run?source=missing").status, 404);
    CHECK_EQ(http_request(server.port(), "GET", "/api/property?key=zzz").status, 404);

    server.stop();
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
                if (dd::str::contains(t.token, "delinquen")) tax_has_delinquent = true;
                CHECK(t.lift > 1.0); // above corpus-average frequency by definition
            }
        }
    }
    CHECK(tax_has_delinquent);
}

TEST(pipeline_resolves_across_sources) {
    // Two different Millbrook sources describe the same parcels: the tax roll
    // and the assessment roll. Records must land on the same properties.
    const std::string root = fresh_dir("pipe_relations");
    dd::store::Store store{root};
    dd::pipeline::Pipeline pipeline{store, test_classifier()};

    const dd::store::Source tax =
        store.add_source("Millbrook Tax", "data/fixtures/millbrook_tax.html", "Millbrook County");
    const dd::store::Source assessor = store.add_source(
        "Millbrook Assessor", "data/fixtures/millbrook_assessor.html", "Millbrook County");
    CHECK(pipeline.run_source(tax).ok);
    const dd::store::RunRecord second = pipeline.run_source(assessor);
    CHECK(second.ok);
    CHECK_EQ(second.classification, "assessor_roll");

    // Still six properties, each now carrying evidence from both sources.
    const std::vector<std::string> keys = store.property_keys();
    CHECK_EQ(keys.size(), std::size_t{6});
    for (const std::string& key : keys) {
        const std::vector<dd::events::PropertyEvent> evs = store.events_for(key);
        CHECK_EQ(evs.size(), std::size_t{2});
        CHECK(evs[0].source_id != evs[1].source_id);
        // The assessment is evidence only: the distress state stays put.
        CHECK(dd::events::reduce(evs).state == dd::events::State::TaxDelinquent);
    }
}

TEST(server_schema_model_benchmark_endpoints) {
    const std::string root = fresh_dir("server_v2");
    dd::store::Store store{root};
    dd::pipeline::Pipeline pipeline{store, test_classifier()};
    dd::server::Server server{store, pipeline, dd::server::Options{"127.0.0.1", 0, "web"}};
    server.start();

    // Model introspection.
    const dd::json::Value model =
        dd::json::parse(http_request(server.port(), "GET", "/api/model").body);
    CHECK_EQ(model.find("classes")->items().size(), std::size_t{8});
    CHECK(model.find("vocabulary")->as_number() > 100);
    CHECK(model.find("classes")->items()[0].find("top_tokens")->items().size() >= 1);

    // Schema before any run: null snapshot, not an invented one.
    const HttpReply added = http_request(
        server.port(), "POST", "/api/sources",
        R"({"name": "Millbrook", "url": "data/fixtures/millbrook_tax.html", "jurisdiction": "Millbrook County"})");
    const std::string sid = dd::json::parse(added.body).find("id")->as_string();
    const dd::json::Value empty_schema = dd::json::parse(
        http_request(server.port(), "GET", "/api/schema?source=" + sid).body);
    CHECK(empty_schema.find("snapshot")->is_null());
    CHECK_EQ(http_request(server.port(), "GET", "/api/schema?source=zzz").status, 404);

    // After a run the schema view carries the dialect and the evidence.
    http_request(server.port(), "POST", "/api/run?source=" + sid);
    const dd::json::Value schema = dd::json::parse(
        http_request(server.port(), "GET", "/api/schema?source=" + sid).body);
    const dd::json::Value* snapshot = schema.find("snapshot");
    CHECK(snapshot->is_object());
    CHECK_EQ(snapshot->find("format")->as_string(), "html");
    CHECK(snapshot->find("labels")->items().size() >= 5);
    CHECK_EQ(snapshot->find("classification")->find("label")->as_string(), "tax_delinquency");
    CHECK_EQ(snapshot->find("classification")->find("distribution")->items().size(),
             std::size_t{8});
    const dd::json::Value* fields = snapshot->find("mapping")->find("fields");
    CHECK(fields->items().size() >= 4);
    const dd::json::Value& first_field = fields->items()[0];
    CHECK(first_field.find("label_similarity")->as_number() > 0.0);
    CHECK(first_field.find("value_pass_rate")->as_number() > 0.0);
    CHECK(snapshot->find("field_rates")->is_object());

    // Records endpoint still returns just the records array.
    const dd::json::Value records = dd::json::parse(
        http_request(server.port(), "GET", "/api/records?source=" + sid).body);
    CHECK_EQ(records.items().size(), std::size_t{6});

    // Benchmark: real runs, measured.
    const dd::json::Value bench = dd::json::parse(
        http_request(server.port(), "POST", "/api/benchmark?rounds=3").body);
    CHECK_NEAR(bench.find("rounds")->as_number(), 3.0, 1e-9);
    CHECK_NEAR(bench.find("runs")->as_number(), 3.0, 1e-9); // one source, three rounds
    CHECK_NEAR(bench.find("ok_runs")->as_number(), 3.0, 1e-9);
    CHECK_NEAR(bench.find("records_processed")->as_number(), 18.0, 1e-9);
    CHECK_NEAR(bench.find("events_new")->as_number(), 0.0, 1e-9); // dedup absorbs reruns
    CHECK(bench.find("total_ms")->as_number() > 0.0);
    CHECK(bench.find("records_per_sec")->as_number() > 0.0);

    // Those benchmark runs are real run records.
    const dd::json::Value runs =
        dd::json::parse(http_request(server.port(), "GET", "/api/runs?limit=100").body);
    CHECK_EQ(runs.items().size(), std::size_t{4});

    server.stop();
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
