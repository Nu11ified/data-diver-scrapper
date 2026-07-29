#include "dd/parse/query.hpp"

#include "dd/core/core.hpp"

#include <algorithm>

namespace dd::html {
DfsIterator& DfsIterator::operator++() {
    if (node_ == nullptr) return *this;
    if (!node_->children.empty()) {
        node_ = node_->children.front().get();
        return *this;
    }
    const Node* current = node_;
    while (current != root_ && current->parent != nullptr) {
        const Node* parent = current->parent;
        const auto it = std::find_if(
            parent->children.begin(), parent->children.end(),
            [&](const NodePtr& child) { return child.get() == current; });
        if (it != parent->children.end() && std::next(it) != parent->children.end()) {
            node_ = std::next(it)->get();
            return *this;
        }
        current = parent;
    }
    node_ = nullptr;
    return *this;
}

DfsIterator DfsIterator::operator++(int) {
    DfsIterator before = *this;
    ++*this;
    return before;
}

DfsRange dfs(const Node* root) { return DfsRange{root}; }
DfsRange dfs(const Document& document) { return DfsRange{document.root()}; }

Selector Selector::operator&&(const Selector& other) const {
    return Selector{[a = *this, b = other](const Node& n) { return a(n) && b(n); }};
}

Selector Selector::operator||(const Selector& other) const {
    return Selector{[a = *this, b = other](const Node& n) { return a(n) || b(n); }};
}

Selector Selector::operator!() const {
    return Selector{[a = *this](const Node& n) { return !a(n); }};
}

Selector tag(std::string name) {
    return Selector{[name = std::move(name)](const Node& n) {
        return n.kind == Node::Kind::Element && n.tag == name;
    }};
}

Selector has_attr(std::string name) {
    return Selector{
        [name = std::move(name)](const Node& n) { return n.attr(name) != nullptr; }};
}

Selector attr_equals(std::string name, std::string value) {
    return Selector{[name = std::move(name), value = std::move(value)](const Node& n) {
        const std::string* found = n.attr(name);
        return found != nullptr && *found == value;
    }};
}

Selector has_class(std::string name) {
    return Selector{[name = std::move(name)](const Node& n) {
        const std::vector<std::string> classes = n.classes();
        return std::find(classes.begin(), classes.end(), name) != classes.end();
    }};
}

Selector text_contains(std::string needle) {
    return Selector{[needle = str::to_lower(needle)](const Node& n) {
        return str::contains(str::to_lower(n.own_text()), needle);
    }};
}

Selector is_element() {
    return Selector{[](const Node& n) { return n.kind == Node::Kind::Element; }};
}

std::vector<const Node*> select(const Node* root, const Selector& selector) {
    std::vector<const Node*> out;
    for (const Node& node : dfs(root)) {
        if (selector(node)) out.push_back(&node);
    }
    return out;
}

std::vector<const Node*> select(const Document& document, const Selector& selector) {
    return select(document.root(), selector);
}

const Node* select_first(const Node* root, const Selector& selector) {
    for (const Node& node : dfs(root)) {
        if (selector(node)) return &node;
    }
    return nullptr;
}

namespace {
struct Compound {
    std::string tag;                                        // empty means universal
    std::vector<std::string> classes;
    std::string id;
    std::vector<std::pair<std::string, std::string>> attrs;  // value empty means presence
    std::vector<bool> attr_has_value;
};

enum class Combinator { Descendant, Child };

struct Chain {
    std::vector<Compound> steps;
    std::vector<Combinator> combinators;  // size() == steps.size() - 1
};

bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_';
}

std::string read_name(std::string_view text, std::size_t& i) {
    const std::size_t start = i;
    while (i < text.size() && is_name_char(text[i])) ++i;
    if (i == start) throw Error{"selector: expected a name at offset " + std::to_string(start)};
    return std::string{text.substr(start, i - start)};
}

Compound parse_compound(std::string_view text, std::size_t& i) {
    Compound out;
    bool any = false;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '*') {
            ++i;
            any = true;
        } else if (c == '.') {
            ++i;
            out.classes.push_back(str::to_lower(read_name(text, i)));
            any = true;
        } else if (c == '#') {
            ++i;
            out.id = read_name(text, i);
            any = true;
        } else if (c == '[') {
            ++i;
            const std::string name = str::to_lower(read_name(text, i));
            std::string value;
            bool has_value = false;
            if (i < text.size() && text[i] == '=') {
                ++i;
                has_value = true;
                char quote = 0;
                if (i < text.size() && (text[i] == '"' || text[i] == '\'')) quote = text[i++];
                const std::size_t start = i;
                while (i < text.size() && (quote != 0 ? text[i] != quote : text[i] != ']')) ++i;
                value = std::string{text.substr(start, i - start)};
                if (quote != 0) {
                    if (i >= text.size()) throw Error{"selector: unterminated attribute value"};
                    ++i;
                }
            }
            if (i >= text.size() || text[i] != ']') throw Error{"selector: expected ']'"};
            ++i;
            out.attrs.emplace_back(name, value);
            out.attr_has_value.push_back(has_value);
            any = true;
        } else if (is_name_char(c)) {
            if (!out.tag.empty() || any) break;  // a second bare name starts a new compound
            out.tag = str::to_lower(read_name(text, i));
            any = true;
        } else {
            break;
        }
    }
    if (!any) throw Error{"selector: empty compound at offset " + std::to_string(i)};
    return out;
}

Chain parse_chain(std::string_view text) {
    Chain chain;
    std::size_t i = 0;
    const auto skip_ws = [&] {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n')) ++i;
    };
    skip_ws();
    chain.steps.push_back(parse_compound(text, i));
    while (i < text.size()) {
        const std::size_t before = i;
        skip_ws();
        const bool had_space = i > before;
        if (i >= text.size()) break;
        Combinator combinator = Combinator::Descendant;
        if (text[i] == '>') {
            combinator = Combinator::Child;
            ++i;
            skip_ws();
        } else if (!had_space) {
            throw Error{"selector: expected a combinator at offset " + std::to_string(i)};
        }
        if (i >= text.size()) break;
        chain.combinators.push_back(combinator);
        chain.steps.push_back(parse_compound(text, i));
    }
    return chain;
}

bool matches_compound(const Node& node, const Compound& compound) {
    if (node.kind != Node::Kind::Element) return false;
    if (!compound.tag.empty() && str::to_lower(node.tag) != compound.tag) return false;
    if (!compound.id.empty()) {
        const std::string* id = node.attr("id");
        if (id == nullptr || *id != compound.id) return false;
    }
    if (!compound.classes.empty()) {
        const std::vector<std::string> classes = node.classes();
        for (const std::string& wanted : compound.classes) {
            bool found = false;
            for (const std::string& actual : classes) {
                if (str::to_lower(actual) == wanted) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
    }
    for (std::size_t a = 0; a < compound.attrs.size(); ++a) {
        const std::string* value = node.attr(compound.attrs[a].first);
        if (value == nullptr) return false;
        if (compound.attr_has_value[a] && *value != compound.attrs[a].second) return false;
    }
    return true;
}

bool matches_at(const Node* node, const Chain& chain, std::size_t step) {
    if (node == nullptr || !matches_compound(*node, chain.steps[step])) return false;
    if (step == 0) return true;
    if (chain.combinators[step - 1] == Combinator::Child) {
        return matches_at(node->parent, chain, step - 1);
    }
    for (const Node* ancestor = node->parent; ancestor != nullptr; ancestor = ancestor->parent) {
        if (matches_at(ancestor, chain, step - 1)) return true;
    }
    return false;
}
} // namespace

Selector css(std::string_view selector) {
    std::vector<Chain> chains;
    std::size_t start = 0;
    while (start <= selector.size()) {
        const std::size_t comma = selector.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? selector.size() : comma;
        const std::string_view part = selector.substr(start, end - start);
        if (str::trim(part).empty()) throw Error{"selector: empty group"};
        chains.push_back(parse_chain(part));
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    if (chains.empty()) throw Error{"selector: nothing to match"};
    return Selector{[chains = std::move(chains)](const Node& node) {
        for (const Chain& chain : chains) {
            if (matches_at(&node, chain, chain.steps.size() - 1)) return true;
        }
        return false;
    }};
}

std::vector<const Node*> query_all(const Node* root, std::string_view selector) {
    return select(root, css(selector));
}

std::vector<const Node*> query_all(const Document& document, std::string_view selector) {
    return select(document.root(), css(selector));
}

const Node* query(const Node* root, std::string_view selector) {
    return select_first(root, css(selector));
}

const Node* query(const Document& document, std::string_view selector) {
    return select_first(document.root(), css(selector));
}

const Node* closest(const Node* node, const Selector& selector) {
    for (const Node* current = node; current != nullptr; current = current->parent) {
        if (selector(*current)) return current;
    }
    return nullptr;
}

std::vector<const Node*> element_children(const Node* node) {
    std::vector<const Node*> out;
    if (node == nullptr) return out;
    for (const NodePtr& child : node->children) {
        if (child->kind == Node::Kind::Element) out.push_back(child.get());
    }
    return out;
}

const Node* sibling(const Node* node, bool forward) {
    if (node == nullptr || node->parent == nullptr) return nullptr;
    const std::vector<const Node*> siblings = element_children(node->parent);
    for (std::size_t i = 0; i < siblings.size(); ++i) {
        if (siblings[i] != node) continue;
        if (forward) return i + 1 < siblings.size() ? siblings[i + 1] : nullptr;
        return i > 0 ? siblings[i - 1] : nullptr;
    }
    return nullptr;
}

const Node* next_element(const Node* node) { return sibling(node, true); }
const Node* previous_element(const Node* node) { return sibling(node, false); }
} // namespace dd::html
