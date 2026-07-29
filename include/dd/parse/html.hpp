#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dd::html {
struct Attribute {
    std::string name;
    std::string value;
};

struct Node;
using NodePtr = std::unique_ptr<Node>;

struct Node {
    enum class Kind { Document, Element, Text };

    Kind kind = Kind::Element;
    std::string tag;
    std::vector<Attribute> attrs;
    std::string text;
    std::vector<NodePtr> children;
    Node* parent = nullptr;

    const std::string* attr(std::string_view name) const;
    std::vector<std::string> classes() const;

    std::string text_content() const;

    std::string own_text() const;

    std::string signature() const;

    std::size_t element_child_count() const;
};

class Document {
public:
    explicit Document(NodePtr root) : root_{std::move(root)} {}

    const Node* root() const noexcept { return root_.get(); }
    std::string title() const;
    std::vector<std::string> headings() const;
    std::vector<const Node*> find_all(std::string_view tag) const;
    std::string text() const;

private:
    NodePtr root_;
};

Document parse(std::string_view html);
std::string decode_entities(std::string_view s);
void walk(const Node* node, const std::function<void(const Node*)>& visit);
} // namespace dd::html
