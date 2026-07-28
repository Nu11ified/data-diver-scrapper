// Behaviour tests for the Data Diver engine. Each test exercises a module
// through its public interface only; ctest runs this binary from the source
// tree so fixtures resolve by relative path.

#include "dd/core.hpp"
#include "dd/csv.hpp"
#include "dd/html.hpp"
#include "dd/json.hpp"

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
