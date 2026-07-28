// Behaviour tests for the Data Diver engine. Each test exercises a module
// through its public interface only; ctest runs this binary from the source
// tree so fixtures resolve by relative path.

#include "dd/classify.hpp"
#include "dd/core.hpp"
#include "dd/csv.hpp"
#include "dd/document.hpp"
#include "dd/features.hpp"
#include "dd/fetch.hpp"
#include "dd/html.hpp"
#include "dd/json.hpp"
#include "dd/metrics.hpp"
#include "dd/model.hpp"
#include "dd/pdf.hpp"

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
