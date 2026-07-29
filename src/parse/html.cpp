#include "dd/parse/html.hpp"

#include "dd/core/core.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace dd::html {
namespace {
bool is_void_tag(std::string_view tag) {
    static constexpr std::array<std::string_view, 14> kVoid = {
        "area", "base", "br",    "col",    "embed",  "hr",  "img",
        "input", "link", "meta", "param",  "source", "track", "wbr"};
    return std::find(kVoid.begin(), kVoid.end(), tag) != kVoid.end();
}

bool is_rawtext_tag(std::string_view tag) { return tag == "script" || tag == "style"; }

bool closes_same(std::string_view tag) {
    static constexpr std::array<std::string_view, 6> kSame = {"li", "tr", "td", "th", "option",
                                                              "p"};
    return std::find(kSame.begin(), kSame.end(), tag) != kSame.end();
}

struct NamedEntity {
    std::string_view name;
    std::string_view utf8;
};

constexpr std::array<NamedEntity, 12> kEntities = {{
    {"amp", "&"},
    {"lt", "<"},
    {"gt", ">"},
    {"quot", "\""},
    {"apos", "'"},
    {"nbsp", " "},
    {"ndash", "\xE2\x80\x93"},
    {"mdash", "\xE2\x80\x94"},
    {"copy", "\xC2\xA9"},
    {"reg", "\xC2\xAE"},
    {"rsquo", "\xE2\x80\x99"},
    {"lsquo", "\xE2\x80\x98"},
}};

void append_codepoint(std::string& out, unsigned long code) {
    if (code == 0 || code > 0x10FFFF) return;
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

class TreeBuilder {
public:
    explicit TreeBuilder(std::string_view input) : input_{input} {
        root_ = std::make_unique<Node>();
        root_->kind = Node::Kind::Document;
        root_->tag = "#document";
        stack_.push_back(root_.get());
    }

    NodePtr run() {
        while (pos_ < input_.size()) {
            const std::size_t lt = input_.find('<', pos_);
            if (lt == std::string_view::npos) {
                add_text(input_.substr(pos_));
                break;
            }
            if (lt > pos_) add_text(input_.substr(pos_, lt - pos_));
            pos_ = lt;
            consume_markup();
        }
        return std::move(root_);
    }

private:
    void add_text(std::string_view raw) {
        const std::string decoded = decode_entities(raw);
        if (str::trim(decoded).empty()) return;
        auto node = std::make_unique<Node>();
        node->kind = Node::Kind::Text;
        node->text = decoded;
        node->parent = stack_.back();
        stack_.back()->children.push_back(std::move(node));
    }

    void consume_markup() {
        if (input_.compare(pos_, 4, "<!--") == 0) {
            const std::size_t end = input_.find("-->", pos_ + 4);
            pos_ = end == std::string_view::npos ? input_.size() : end + 3;
            return;
        }
        if (pos_ + 1 < input_.size() && (input_[pos_ + 1] == '!' || input_[pos_ + 1] == '?')) {
            const std::size_t end = input_.find('>', pos_);
            pos_ = end == std::string_view::npos ? input_.size() : end + 1;
            return;
        }
        if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '/') {
            consume_close_tag();
            return;
        }
        if (pos_ + 1 >= input_.size() ||
            std::isalpha(static_cast<unsigned char>(input_[pos_ + 1])) == 0) {
            add_text("<");
            ++pos_;
            return;
        }
        consume_open_tag();
    }

    void consume_close_tag() {
        pos_ += 2; // "</"
        const std::string tag = str::to_lower(read_name());
        const std::size_t end = input_.find('>', pos_);
        pos_ = end == std::string_view::npos ? input_.size() : end + 1;

        for (std::size_t i = stack_.size(); i-- > 1;) {
            if (stack_[i]->tag == tag) {
                stack_.resize(i);
                return;
            }
        }
    }

    void consume_open_tag() {
        ++pos_; // '<'
        const std::string tag = str::to_lower(read_name());
        auto node = std::make_unique<Node>();
        node->kind = Node::Kind::Element;
        node->tag = tag;
        read_attributes(*node);

        bool self_closing = false;
        if (pos_ < input_.size() && input_[pos_] == '/') {
            self_closing = true;
            ++pos_;
        }
        if (pos_ < input_.size() && input_[pos_] == '>') ++pos_;

        if (closes_same(tag)) implicitly_close(tag);

        Node* raw = node.get();
        node->parent = stack_.back();
        stack_.back()->children.push_back(std::move(node));

        if (self_closing || is_void_tag(tag)) return;

        if (is_rawtext_tag(tag)) {
            consume_rawtext(*raw, tag);
            return;
        }
        stack_.push_back(raw);
    }

    void implicitly_close(std::string_view tag) {
        for (std::size_t i = stack_.size(); i-- > 1;) {
            if (stack_[i]->tag == tag) {
                stack_.resize(i);
                return;
            }
            if (stack_[i]->tag == "table" || stack_[i]->tag == "ul" || stack_[i]->tag == "ol" ||
                stack_[i]->tag == "select" || stack_[i]->tag == "div") {
                return;
            }
        }
    }

    void consume_rawtext(Node& element, std::string_view tag) {
        const std::string close = "</" + std::string{tag};
        std::size_t end = pos_;
        while (true) {
            end = input_.find(close, end);
            if (end == std::string_view::npos) {
                end = input_.size();
                break;
            }
            const std::size_t after = end + close.size();
            if (after >= input_.size() || input_[after] == '>' ||
                std::isspace(static_cast<unsigned char>(input_[after])) != 0) {
                break;
            }
            ++end;
        }
        if (end > pos_) {
            auto text = std::make_unique<Node>();
            text->kind = Node::Kind::Text;
            text->text = std::string{input_.substr(pos_, end - pos_)};
            text->parent = &element;
            element.children.push_back(std::move(text));
        }
        const std::size_t gt = input_.find('>', end);
        pos_ = gt == std::string_view::npos ? input_.size() : gt + 1;
    }

    std::string read_name() {
        const std::size_t start = pos_;
        while (pos_ < input_.size()) {
            const char c = input_[pos_];
            if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' ||
                c == ':') {
                ++pos_;
                continue;
            }
            break;
        }
        return std::string{input_.substr(start, pos_ - start)};
    }

    void skip_spaces() {
        while (pos_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[pos_])) != 0) {
            ++pos_;
        }
    }

    void read_attributes(Node& node) {
        while (true) {
            skip_spaces();
            if (pos_ >= input_.size()) return;
            const char c = input_[pos_];
            if (c == '>' || c == '/') return;
            const std::string name = str::to_lower(read_name());
            if (name.empty()) {
                ++pos_; // Unexpected character: skip so we always progress.
                continue;
            }
            skip_spaces();
            std::string value;
            if (pos_ < input_.size() && input_[pos_] == '=') {
                ++pos_;
                skip_spaces();
                if (pos_ < input_.size() && (input_[pos_] == '"' || input_[pos_] == '\'')) {
                    const char q = input_[pos_++];
                    const std::size_t end = input_.find(q, pos_);
                    if (end == std::string_view::npos) {
                        value = decode_entities(input_.substr(pos_));
                        pos_ = input_.size();
                    } else {
                        value = decode_entities(input_.substr(pos_, end - pos_));
                        pos_ = end + 1;
                    }
                } else {
                    const std::size_t start = pos_;
                    while (pos_ < input_.size() && input_[pos_] != '>' && input_[pos_] != '/' &&
                           std::isspace(static_cast<unsigned char>(input_[pos_])) == 0) {
                        ++pos_;
                    }
                    value = decode_entities(input_.substr(start, pos_ - start));
                }
            }
            node.attrs.push_back(Attribute{name, value});
        }
    }

    std::string_view input_;
    std::size_t pos_ = 0;
    NodePtr root_;
    std::vector<Node*> stack_;
};
} // namespace

std::string decode_entities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '&') {
            out.push_back(s[i++]);
            continue;
        }
        const std::size_t semi = s.find(';', i + 1);
        if (semi == std::string_view::npos || semi - i > 10) {
            out.push_back(s[i++]);
            continue;
        }
        const std::string_view body = s.substr(i + 1, semi - i - 1);
        if (!body.empty() && body[0] == '#') {
            unsigned long code = 0;
            bool ok = false;
            if (body.size() > 1 && (body[1] == 'x' || body[1] == 'X')) {
                for (std::size_t j = 2; j < body.size(); ++j) {
                    const char c = body[j];
                    code <<= 4;
                    if (c >= '0' && c <= '9') code |= static_cast<unsigned long>(c - '0');
                    else if (c >= 'a' && c <= 'f') code |= static_cast<unsigned long>(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') code |= static_cast<unsigned long>(c - 'A' + 10);
                    else { code = 0; break; }
                    ok = true;
                }
            } else {
                for (std::size_t j = 1; j < body.size(); ++j) {
                    const char c = body[j];
                    if (c < '0' || c > '9') { ok = false; break; }
                    code = code * 10 + static_cast<unsigned long>(c - '0');
                    ok = true;
                }
            }
            if (ok && code != 0) {
                append_codepoint(out, code);
                i = semi + 1;
                continue;
            }
        } else {
            const auto it = std::find_if(kEntities.begin(), kEntities.end(),
                                         [&](const NamedEntity& e) { return e.name == body; });
            if (it != kEntities.end()) {
                out.append(it->utf8);
                i = semi + 1;
                continue;
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}

const std::string* Node::attr(std::string_view name) const {
    for (const Attribute& a : attrs) {
        if (a.name == name) return &a.value;
    }
    return nullptr;
}

std::vector<std::string> Node::classes() const {
    const std::string* value = attr("class");
    if (value == nullptr) return {};
    std::vector<std::string> out;
    for (const std::string& part : str::split(*value, ' ')) {
        const std::string cleaned = str::trim(part);
        if (!cleaned.empty()) out.push_back(str::to_lower(cleaned));
    }
    return out;
}

std::string Node::text_content() const {
    std::string out;
    walk(this, [&](const Node* n) {
        if (n->kind != Kind::Text) return;
        if (!out.empty()) out.push_back(' ');
        out += n->text;
    });
    return str::collapse_ws(out);
}

std::string Node::own_text() const {
    std::string out;
    for (const NodePtr& child : children) {
        if (child->kind != Kind::Text) continue;
        if (!out.empty()) out.push_back(' ');
        out += child->text;
    }
    return str::collapse_ws(out);
}

std::string Node::signature() const {
    std::vector<std::string> cls = classes();
    std::sort(cls.begin(), cls.end());
    std::string out = tag;
    for (const std::string& c : cls) {
        out.push_back('.');
        out += c;
    }
    return out;
}

std::size_t Node::element_child_count() const {
    std::size_t n = 0;
    for (const NodePtr& child : children) {
        if (child->kind == Kind::Element) ++n;
    }
    return n;
}

std::string Document::title() const {
    std::string result;
    bool found = false;
    walk(root(), [&](const Node* n) {
        if (found || n->kind != Node::Kind::Element || n->tag != "title") return;
        result = n->text_content();
        found = true;
    });
    return result;
}

std::vector<std::string> Document::headings() const {
    static constexpr std::array<std::string_view, 6> kHeadingTags = {
        "h1", "h2", "h3", "h4", "h5", "h6"};
    std::vector<std::string> out;
    walk(root(), [&](const Node* n) {
        if (n->kind != Node::Kind::Element) return;
        if (std::find(kHeadingTags.begin(), kHeadingTags.end(), n->tag) == kHeadingTags.end())
            return;
        const std::string text = n->text_content();
        if (!text.empty()) out.push_back(text);
    });
    return out;
}

std::vector<const Node*> Document::find_all(std::string_view tag_name) const {
    std::vector<const Node*> out;
    walk(root(), [&](const Node* n) {
        if (n->kind == Node::Kind::Element && n->tag == tag_name) out.push_back(n);
    });
    return out;
}

std::string Document::text() const { return root_->text_content(); }

void walk(const Node* node, const std::function<void(const Node*)>& visit) {
    if (node == nullptr) return;
    visit(node);
    for (const NodePtr& child : node->children) walk(child.get(), visit);
}

Document parse(std::string_view html) {
    TreeBuilder builder{html};
    return Document{builder.run()};
}
} // namespace dd::html
