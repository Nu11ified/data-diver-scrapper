#pragma once

#include "dd/parse/html.hpp"

#include <iterator>
#include <string>
#include <vector>

namespace dd::html {
class DfsIterator {
public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = const Node;
    using difference_type = std::ptrdiff_t;
    using pointer = const Node*;
    using reference = const Node&;

    DfsIterator() = default;
    DfsIterator(const Node* node, const Node* root) : node_{node}, root_{root} {}

    reference operator*() const { return *node_; }
    pointer operator->() const { return node_; }
    DfsIterator& operator++();
    DfsIterator operator++(int);
    bool operator==(const DfsIterator& other) const { return node_ == other.node_; }
    bool operator!=(const DfsIterator& other) const { return node_ != other.node_; }

private:
    const Node* node_ = nullptr;
    const Node* root_ = nullptr;
};

class DfsRange {
public:
    explicit DfsRange(const Node* root) : root_{root} {}
    DfsIterator begin() const { return DfsIterator{root_, root_}; }
    DfsIterator end() const { return DfsIterator{nullptr, root_}; }

private:
    const Node* root_ = nullptr;
};

DfsRange dfs(const Node* root);
DfsRange dfs(const Document& document);

class Selector {
public:
    using Fn = std::function<bool(const Node&)>;
    explicit Selector(Fn fn) : fn_{std::move(fn)} {}

    bool operator()(const Node& node) const { return fn_(node); }
    Selector operator&&(const Selector& other) const;
    Selector operator||(const Selector& other) const;
    Selector operator!() const;

private:
    Fn fn_;
};

Selector tag(std::string name);
Selector has_attr(std::string name);
Selector attr_equals(std::string name, std::string value);
Selector has_class(std::string name);
Selector text_contains(std::string needle);  // case-insensitive, own text
Selector is_element();

std::vector<const Node*> select(const Node* root, const Selector& selector);
std::vector<const Node*> select(const Document& document, const Selector& selector);
const Node* select_first(const Node* root, const Selector& selector);

/// A CSS subset: type, universal, .class, #id, [attr], [attr="value"],
/// compounds, descendant and child combinators, and comma groups.
/// Throws Error on a malformed selector rather than matching nothing.
Selector css(std::string_view selector);

std::vector<const Node*> query_all(const Node* root, std::string_view selector);
std::vector<const Node*> query_all(const Document& document, std::string_view selector);
const Node* query(const Node* root, std::string_view selector);
const Node* query(const Document& document, std::string_view selector);

const Node* closest(const Node* node, const Selector& selector);
std::vector<const Node*> element_children(const Node* node);
const Node* next_element(const Node* node);
const Node* previous_element(const Node* node);
} // namespace dd::html
