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

} // namespace dd::html
