// SPDX-License-Identifier: MIT
// One chat + tool-calling interface over Anthropic (Claude), OpenAI (GPT, and any
// OpenAI-compatible server), Google Gemini, and Ollama. Streams responses.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "json.hpp"

namespace ai {

enum class Provider { Anthropic = 0, OpenAI = 1, Gemini = 2, Ollama = 3 };
constexpr int kProviderCount = 4;
const char* provider_name(Provider p);        // "Claude (Anthropic)" …
const char* provider_short(Provider p);       // "anthropic" … (settings keys)
std::string default_base_url(Provider p);
bool needs_key(Provider p);

struct ToolDef {
    std::string name, description;
    json::Value schema;  // JSON Schema for the arguments (type: object)
};
struct ToolCall {
    std::string id, name;
    json::Value input;
};
struct ToolResult {
    std::string id, name, content;
    bool error = false;
};
struct Message {
    enum Role { User, Assistant, Tools } role = User;
    std::string text;
    std::vector<ToolCall> calls;      // Assistant
    std::vector<ToolResult> results;  // Tools
    json::Value raw;                  // provider-native assistant content to echo back (Gemini thought signatures)
};
struct Config {
    Provider provider = Provider::Anthropic;
    std::string key, model, base_url;
    int max_tokens = 8192;
};
struct Turn {
    std::string text, stop_reason, error;
    std::vector<ToolCall> calls;
    json::Value raw;
};

// One model turn. `on_text` gets streamed text; return false from `keep_going` to cancel.
Turn complete(const Config& cfg, const std::string& system, const std::vector<Message>& history, const std::vector<ToolDef>& tools,
              const std::function<void(const std::string&)>& on_text, const std::function<bool()>& keep_going);
// Model ids the account can use (throws on network/auth errors).
std::vector<std::string> list_models(const Config& cfg);

}  // namespace ai
