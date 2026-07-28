#pragma once

#include <cstdint>
#include <string>

// Baseline client for the benchmark: one chat completion against any
// OpenAI-compatible endpoint, configured entirely from the environment so
// no provider is hardcoded and no key ever touches the repo.
namespace dd::llm {

struct Config {
    std::string endpoint;  // DD_LLM_ENDPOINT, e.g. .../v1/chat/completions
    std::string key;       // DD_LLM_KEY
    std::string model;     // DD_LLM_MODEL
    double price_in = 0.0;   // DD_LLM_PRICE_IN, dollars per 1M input tokens
    double price_out = 0.0;  // DD_LLM_PRICE_OUT, dollars per 1M output tokens

    bool ready() const noexcept { return !endpoint.empty() && !model.empty(); }
};

Config config_from_env();

struct Completion {
    bool ok = false;
    std::string text;  // assistant message content
    std::int64_t tokens_in = 0;
    std::int64_t tokens_out = 0;
    double total_ms = 0.0;
    std::string error;
};

// Builds the chat request body for one user prompt at temperature 0.
std::string request_body(const Config& config, const std::string& prompt);

// Parses a chat-completions response body into content and token usage.
Completion parse_response(const std::string& body);

// request_body -> POST -> parse_response, with transfer time measured.
Completion complete(const Config& config, const std::string& prompt);

} // namespace dd::llm
