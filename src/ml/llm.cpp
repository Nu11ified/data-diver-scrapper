#include "dd/ml/llm.hpp"

#include "dd/core/core.hpp"
#include "dd/core/json.hpp"
#include "dd/net/fetch.hpp"

#include <cstdlib>

namespace dd::llm {
namespace {

std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value == nullptr ? fallback : value;
}

} // namespace

Config config_from_env() {
    Config c;
    c.endpoint = env_or("DD_LLM_ENDPOINT", "https://api.openai.com/v1/chat/completions");
    c.key = env_or("DD_LLM_KEY", "");
    c.model = env_or("DD_LLM_MODEL", "");
    c.price_in = std::atof(env_or("DD_LLM_PRICE_IN", "0").c_str());
    c.price_out = std::atof(env_or("DD_LLM_PRICE_OUT", "0").c_str());
    return c;
}

std::string request_body(const Config& config, const std::string& prompt) {
    json::Writer w;
    w.begin_object();
    w.field("model", config.model);
    w.field("temperature", 0.0);
    w.key("messages");
    w.begin_array();
    w.begin_object();
    w.field("role", "user");
    w.field("content", prompt);
    w.end_object();
    w.end_array();
    w.end_object();
    return w.take();
}

Completion parse_response(const std::string& body) {
    Completion out;
    json::Value root;
    try {
        root = json::parse(body);
    } catch (const Error& e) {
        out.error = std::string{"unparseable response: "} + e.what();
        return out;
    }
    if (const json::Value* err = root.find("error"); err != nullptr) {
        const json::Value* message = err->find("message");
        out.error = message != nullptr ? message->as_string() : "provider returned an error";
        return out;
    }
    const json::Value* choices = root.find("choices");
    if (choices == nullptr || choices->items().empty()) {
        out.error = "response has no choices";
        return out;
    }
    const json::Value* message = choices->items().front().find("message");
    const json::Value* content = message == nullptr ? nullptr : message->find("content");
    if (content == nullptr) {
        out.error = "response has no message content";
        return out;
    }
    out.text = content->as_string();
    if (const json::Value* usage = root.find("usage"); usage != nullptr) {
        const json::Value* in = usage->find("prompt_tokens");
        const json::Value* generated = usage->find("completion_tokens");
        if (in != nullptr) out.tokens_in = static_cast<std::int64_t>(in->as_number());
        if (generated != nullptr) out.tokens_out = static_cast<std::int64_t>(generated->as_number());
    }
    out.ok = true;
    return out;
}

Completion complete(const Config& config, const std::string& prompt) {
    if (!config.ready()) {
        Completion out;
        out.error = "llm baseline not configured: set DD_LLM_MODEL and DD_LLM_KEY "
                    "(DD_LLM_ENDPOINT to change provider)";
        return out;
    }
    fetch::Options options;
    options.timeout_seconds = 120;
    const fetch::Result response =
        fetch::post_json(config.endpoint, request_body(config, prompt), config.key, options);
    if (!response.ok && response.body.empty()) {
        Completion out;
        out.error = response.error;
        out.total_ms = response.total_ms;
        return out;
    }
    Completion out = parse_response(response.body);
    out.total_ms = response.total_ms;
    return out;
}

} // namespace dd::llm
