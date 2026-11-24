// SPDX-License-Identifier: MIT
#include "ai.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>

#include "http.hpp"

namespace ai {

const char* provider_name(Provider p) {
    switch (p) {
        case Provider::Anthropic: return "Claude (Anthropic)";
        case Provider::OpenAI: return "GPT (OpenAI)";
        case Provider::Gemini: return "Gemini (Google)";
        case Provider::Ollama: return "Ollama (local)";
    }
    return "";
}
const char* provider_short(Provider p) {
    switch (p) {
        case Provider::Anthropic: return "anthropic";
        case Provider::OpenAI: return "openai";
        case Provider::Gemini: return "gemini";
        case Provider::Ollama: return "ollama";
    }
    return "";
}
std::string default_base_url(Provider p) {
    switch (p) {
        case Provider::Anthropic: return "https://api.anthropic.com";
        case Provider::OpenAI: return "https://api.openai.com/v1";
        case Provider::Gemini: return "https://generativelanguage.googleapis.com";
        case Provider::Ollama: return "http://localhost:11434";
    }
    return "";
}
bool needs_key(Provider p) { return p != Provider::Ollama; }

static std::string base(const Config& c) {
    std::string b = c.base_url.empty() ? default_base_url(c.provider) : c.base_url;
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b;
}

// ---------------------------------------------------------------- streaming parsers
// Server-sent events: blank-line separated; joins `data:` lines.
struct SseParser {
    std::string buf;
    std::function<void(const std::string& event, const std::string& data)> on_event;
    void feed(std::string_view chunk) {
        buf.append(chunk);
        for (;;) {
            size_t end = buf.find("\n\n");
            size_t end_r = buf.find("\r\n\r\n");
            size_t cut, skip;
            if (end_r != std::string::npos && (end == std::string::npos || end_r < end)) { cut = end_r; skip = 4; }
            else if (end != std::string::npos) { cut = end; skip = 2; }
            else break;
            std::string block = buf.substr(0, cut);
            buf.erase(0, cut + skip);
            std::string event, data;
            size_t p = 0;
            while (p <= block.size()) {
                size_t nl = block.find('\n', p);
                std::string line = block.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("event:", 0) == 0) event = line.substr(line.find_first_not_of(' ', 6));
                else if (line.rfind("data:", 0) == 0) {
                    if (!data.empty()) data += "\n";
                    data += line.size() > 5 ? line.substr(line[5] == ' ' ? 6 : 5) : "";
                }
                if (nl == std::string::npos) break;
                p = nl + 1;
            }
            if (!data.empty() && on_event) on_event(event, data);
        }
    }
};

static std::string error_message(const net::Response& r) {
    try {
        auto v = json::parse(r.body);
        if (v["error"].is_object()) return v["error"]["message"].str("HTTP " + std::to_string(r.status));
        if (v["error"].is_string()) return v["error"].str();
        if (v["message"].is_string()) return v["message"].str();
    } catch (...) {}
    std::string b = r.body.substr(0, 300);
    return "HTTP " + std::to_string(r.status) + (b.empty() ? "" : ": " + b);
}

static json::Value parse_args(const std::string& s) {
    if (s.empty()) return json::Object{};
    try { return json::parse(s); } catch (...) { return json::Object{{"_raw", s}}; }
}

// Gemini's function schema is OpenAPI-flavoured: upper-case type names.
static json::Value gemini_schema(const json::Value& v) {
    if (v.is_object()) {
        json::Object o;
        for (auto& [k, x] : v.obj()) {
            if (k == "additionalProperties" || k == "$schema") continue;
            if (k == "type" && x.is_string()) {
                std::string t = x.str();
                for (auto& ch : t) ch = char(std::toupper((unsigned char)ch));
                o[k] = t;
            } else o[k] = gemini_schema(x);
        }
        return o;
    }
    if (v.is_array()) {
        json::Array a;
        for (auto& x : v.arr()) a.push_back(gemini_schema(x));
        return a;
    }
    return v;
}

// ---------------------------------------------------------------- request bodies
static json::Value anthropic_body(const Config& c, const std::string& system, const std::vector<Message>& h, const std::vector<ToolDef>& tools) {
    json::Array msgs;
    for (auto& m : h) {
        if (m.role == Message::User) msgs.push_back(json::Object{{"role", "user"}, {"content", m.text}});
        else if (m.role == Message::Assistant) {
            json::Array content;
            if (!m.text.empty()) content.push_back(json::Object{{"type", "text"}, {"text", m.text}});
            for (auto& tc : m.calls) content.push_back(json::Object{{"type", "tool_use"}, {"id", tc.id}, {"name", tc.name}, {"input", tc.input}});
            if (content.empty()) content.push_back(json::Object{{"type", "text"}, {"text", "(no response)"}});
            msgs.push_back(json::Object{{"role", "assistant"}, {"content", content}});
        } else {
            json::Array content;
            for (auto& r : m.results)
                content.push_back(json::Object{{"type", "tool_result"}, {"tool_use_id", r.id}, {"content", r.content}, {"is_error", r.error}});
            msgs.push_back(json::Object{{"role", "user"}, {"content", content}});
        }
    }
    json::Array ts;
    for (auto& t : tools) ts.push_back(json::Object{{"name", t.name}, {"description", t.description}, {"input_schema", t.schema}});
    json::Object b{{"model", c.model}, {"max_tokens", c.max_tokens}, {"system", system}, {"messages", msgs}, {"stream", true}};
    if (!ts.empty()) b["tools"] = ts;
    return b;
}

static json::Value openai_messages(const std::string& system, const std::vector<Message>& h, bool ollama) {
    json::Array msgs;
    msgs.push_back(json::Object{{"role", "system"}, {"content", system}});
    for (auto& m : h) {
        if (m.role == Message::User) msgs.push_back(json::Object{{"role", "user"}, {"content", m.text}});
        else if (m.role == Message::Assistant) {
            json::Object a{{"role", "assistant"}, {"content", m.text}};
            if (!m.calls.empty()) {
                json::Array calls;
                for (auto& tc : m.calls) {
                    if (ollama) calls.push_back(json::Object{{"function", json::Object{{"name", tc.name}, {"arguments", tc.input}}}});
                    else calls.push_back(json::Object{{"id", tc.id}, {"type", "function"},
                                                      {"function", json::Object{{"name", tc.name}, {"arguments", tc.input.dump()}}}});
                }
                a["tool_calls"] = calls;
                if (!ollama && m.text.empty()) a["content"] = nullptr;
            }
            msgs.push_back(a);
        } else {
            for (auto& r : m.results) {
                if (ollama) msgs.push_back(json::Object{{"role", "tool"}, {"content", r.content}, {"tool_name", r.name}});
                else msgs.push_back(json::Object{{"role", "tool"}, {"tool_call_id", r.id}, {"content", r.content}});
            }
        }
    }
    return msgs;
}

static json::Value openai_tools(const std::vector<ToolDef>& tools) {
    json::Array ts;
    for (auto& t : tools)
        ts.push_back(json::Object{{"type", "function"}, {"function", json::Object{{"name", t.name}, {"description", t.description}, {"parameters", t.schema}}}});
    return ts;
}

static json::Value gemini_body(const std::string& system, const std::vector<Message>& h, const std::vector<ToolDef>& tools) {
    json::Array contents;
    for (auto& m : h) {
        if (m.role == Message::User) contents.push_back(json::Object{{"role", "user"}, {"parts", json::Array{json::Object{{"text", m.text}}}}});
        else if (m.role == Message::Assistant) {
            if (m.raw.is_array() && m.raw.size()) contents.push_back(json::Object{{"role", "model"}, {"parts", m.raw}});
            else {
                json::Array parts;
                if (!m.text.empty()) parts.push_back(json::Object{{"text", m.text}});
                for (auto& tc : m.calls) parts.push_back(json::Object{{"functionCall", json::Object{{"name", tc.name}, {"args", tc.input}}}});
                if (parts.empty()) parts.push_back(json::Object{{"text", "(no response)"}});
                contents.push_back(json::Object{{"role", "model"}, {"parts", parts}});
            }
        } else {
            json::Array parts;
            for (auto& r : m.results)
                parts.push_back(json::Object{{"functionResponse", json::Object{{"name", r.name}, {"response", json::Object{{r.error ? "error" : "result", r.content}}}}}});
            contents.push_back(json::Object{{"role", "user"}, {"parts", parts}});
        }
    }
    json::Array decls;
    for (auto& t : tools) decls.push_back(json::Object{{"name", t.name}, {"description", t.description}, {"parameters", gemini_schema(t.schema)}});
    json::Object b{{"contents", contents}, {"systemInstruction", json::Object{{"parts", json::Array{json::Object{{"text", system}}}}}}};
    if (!decls.empty()) b["tools"] = json::Array{json::Object{{"functionDeclarations", decls}}};
    return b;
}

// ---------------------------------------------------------------- complete
Turn complete(const Config& c, const std::string& system, const std::vector<Message>& history, const std::vector<ToolDef>& tools,
              const std::function<void(const std::string&)>& on_text, const std::function<bool()>& keep_going) {
    Turn t;
    net::Request rq;
    rq.method = "POST";
    rq.headers["Content-Type"] = "application/json";
    auto emit = [&](const std::string& s) {
        if (s.empty()) return;
        t.text += s;
        if (on_text) on_text(s);
    };
    SseParser sse;
    std::string line_buf;  // Ollama NDJSON
    switch (c.provider) {
        case Provider::Anthropic: {
            rq.url = base(c) + "/v1/messages";
            rq.headers["x-api-key"] = c.key;
            rq.headers["anthropic-version"] = "2023-06-01";
            rq.headers["Accept"] = "text/event-stream";
            rq.body = anthropic_body(c, system, history, tools).dump();
            struct Block { bool tool = false; std::string id, name, json; };
            std::map<int, Block> blocks;
            sse.on_event = [&](const std::string&, const std::string& data) {
                json::Value v;
                try { v = json::parse(data); } catch (...) { return; }
                std::string type = v["type"].str();
                if (type == "content_block_start") {
                    int i = int(v["index"].integer());
                    auto& cb = v["content_block"];
                    if (cb["type"].str() == "tool_use") blocks[i] = {true, cb["id"].str(), cb["name"].str(), ""};
                    else blocks[i] = {};
                } else if (type == "content_block_delta") {
                    int i = int(v["index"].integer());
                    auto& d = v["delta"];
                    if (d["type"].str() == "text_delta") emit(d["text"].str());
                    else if (d["type"].str() == "input_json_delta") blocks[i].json += d["partial_json"].str();
                } else if (type == "content_block_stop") {
                    int i = int(v["index"].integer());
                    auto& b = blocks[i];
                    if (b.tool) t.calls.push_back({b.id, b.name, parse_args(b.json)});
                } else if (type == "message_delta") {
                    if (v["delta"]["stop_reason"].is_string()) t.stop_reason = v["delta"]["stop_reason"].str();
                } else if (type == "error") {
                    t.error = v["error"]["message"].str("The model returned an error.");
                }
            };
            break;
        }
        case Provider::OpenAI: {
            rq.url = base(c) + "/chat/completions";
            if (!c.key.empty()) rq.headers["Authorization"] = "Bearer " + c.key;
            rq.headers["Accept"] = "text/event-stream";
            json::Object b{{"model", c.model}, {"messages", openai_messages(system, history, false)}, {"stream", true}};
            if (!tools.empty()) b["tools"] = openai_tools(tools);
            rq.body = json::Value(b).dump();
            struct Partial { std::string id, name, args; };
            std::map<int, Partial> partial;
            sse.on_event = [&](const std::string&, const std::string& data) {
                if (data == "[DONE]") return;
                json::Value v;
                try { v = json::parse(data); } catch (...) { return; }
                if (v["error"].is_object()) { t.error = v["error"]["message"].str(); return; }
                auto& ch = v["choices"][0];
                auto& d = ch["delta"];
                if (d["content"].is_string()) emit(d["content"].str());
                if (d["tool_calls"].is_array())
                    for (auto& tc : d["tool_calls"].arr()) {
                        auto& p = partial[int(tc["index"].integer())];
                        if (tc["id"].is_string()) p.id = tc["id"].str();
                        if (tc["function"]["name"].is_string()) p.name += tc["function"]["name"].str();
                        if (tc["function"]["arguments"].is_string()) p.args += tc["function"]["arguments"].str();
                    }
                if (ch["finish_reason"].is_string()) t.stop_reason = ch["finish_reason"].str();
            };
            auto r = net::Response{};
            rq.on_data = [&](std::string_view chunk) { sse.feed(chunk); return !keep_going || keep_going(); };
            r = net::request(rq);
            if (r.status >= 400) { t.error = error_message(r); return t; }
            for (auto& [i, p] : partial) t.calls.push_back({p.id.empty() ? "call_" + std::to_string(i) : p.id, p.name, parse_args(p.args)});
            return t;
        }
        case Provider::Gemini: {
            rq.url = base(c) + "/v1beta/models/" + c.model + ":streamGenerateContent?alt=sse";
            rq.headers["x-goog-api-key"] = c.key;
            rq.body = gemini_body(system, history, tools).dump();
            json::Array raw;
            sse.on_event = [&](const std::string&, const std::string& data) {
                json::Value v;
                try { v = json::parse(data); } catch (...) { return; }
                if (v["error"].is_object()) { t.error = v["error"]["message"].str(); return; }
                auto& cand = v["candidates"][0];
                for (auto& p : cand["content"]["parts"].arr()) {
                    raw.push_back(p);
                    if (p["text"].is_string() && !p["thought"].boolean()) emit(p["text"].str());
                    if (p["functionCall"].is_object()) {
                        auto& fc = p["functionCall"];
                        t.calls.push_back({fc["id"].str(fc["name"].str() + "_" + std::to_string(t.calls.size())), fc["name"].str(),
                                           fc["args"].is_object() ? fc["args"] : json::Value(json::Object{})});
                    }
                }
                if (cand["finishReason"].is_string()) t.stop_reason = cand["finishReason"].str();
            };
            rq.on_data = [&](std::string_view chunk) { sse.feed(chunk); return !keep_going || keep_going(); };
            auto r = net::request(rq);
            if (r.status >= 400) { t.error = error_message(r); return t; }
            t.raw = raw;
            return t;
        }
        case Provider::Ollama: {
            rq.url = base(c) + "/api/chat";
            json::Object b{{"model", c.model}, {"messages", openai_messages(system, history, true)}, {"stream", true}};
            if (!tools.empty()) b["tools"] = openai_tools(tools);
            rq.body = json::Value(b).dump();
            rq.on_data = [&](std::string_view chunk) {
                line_buf.append(chunk);
                size_t nl;
                while ((nl = line_buf.find('\n')) != std::string::npos) {
                    std::string line = line_buf.substr(0, nl);
                    line_buf.erase(0, nl + 1);
                    if (line.empty()) continue;
                    json::Value v;
                    try { v = json::parse(line); } catch (...) { continue; }
                    if (v["error"].is_string()) { t.error = v["error"].str(); continue; }
                    auto& m = v["message"];
                    if (m["content"].is_string()) emit(m["content"].str());
                    if (m["tool_calls"].is_array())
                        for (auto& tc : m["tool_calls"].arr())
                            t.calls.push_back({"call_" + std::to_string(t.calls.size()), tc["function"]["name"].str(),
                                               tc["function"]["arguments"].is_object() ? tc["function"]["arguments"] : parse_args(tc["function"]["arguments"].str())});
                    if (v["done"].boolean()) t.stop_reason = v["done_reason"].str("stop");
                }
                return !keep_going || keep_going();
            };
            auto r = net::request(rq);
            if (r.status >= 400) { t.error = error_message(r); return t; }
            return t;
        }
    }
    // Anthropic (shared SSE path)
    rq.on_data = [&](std::string_view chunk) { sse.feed(chunk); return !keep_going || keep_going(); };
    auto r = net::request(rq);
    if (r.status >= 400) t.error = error_message(r);
    return t;
}

std::vector<std::string> list_models(const Config& c) {
    net::Request rq;
    std::vector<std::string> out;
    switch (c.provider) {
        case Provider::Anthropic:
            rq.url = base(c) + "/v1/models?limit=100";
            rq.headers["x-api-key"] = c.key;
            rq.headers["anthropic-version"] = "2023-06-01";
            break;
        case Provider::OpenAI:
            rq.url = base(c) + "/models";
            if (!c.key.empty()) rq.headers["Authorization"] = "Bearer " + c.key;
            break;
        case Provider::Gemini:
            rq.url = base(c) + "/v1beta/models?pageSize=200";
            rq.headers["x-goog-api-key"] = c.key;
            break;
        case Provider::Ollama: rq.url = base(c) + "/api/tags"; break;
    }
    auto r = net::request(rq);
    if (r.status >= 400) throw std::runtime_error(error_message(r));
    auto v = json::parse(r.body);
    if (c.provider == Provider::Ollama) {
        for (auto& m : v["models"].arr()) out.push_back(m["name"].str());
    } else if (c.provider == Provider::Gemini) {
        for (auto& m : v["models"].arr()) {
            bool chat = false;
            for (auto& g : m["supportedGenerationMethods"].arr()) if (g.str() == "generateContent") chat = true;
            std::string n = m["name"].str();
            if (n.rfind("models/", 0) == 0) n = n.substr(7);
            if (chat) out.push_back(n);
        }
    } else {
        for (auto& m : v["data"].arr()) {
            std::string id = m["id"].str();
            // OpenAI lists every model; keep the chat ones
            bool skip = false;
            if (c.provider == Provider::OpenAI)
                for (auto* bad : {"embedding", "whisper", "tts", "dall-e", "moderation", "davinci", "babbage", "transcribe", "image", "realtime", "audio", "search"})
                    if (id.find(bad) != std::string::npos) skip = true;
            if (!skip) out.push_back(id);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace ai
