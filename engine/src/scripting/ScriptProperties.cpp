#include <engine/scripting/ScriptProperties.hpp>
#include <lua.hpp>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <set>

namespace engine {
namespace {
bool Numeric(const StateValue& v) {
    return std::holds_alternative<double>(v.value) || std::holds_alternative<std::int64_t>(v.value) ||
        std::holds_alternative<std::uint64_t>(v.value);
}

struct Token {
    enum Kind { Word, String, Number, Symbol };
    std::string text;
    Kind kind;
    std::size_t line;
};

class Lexer {
    std::string_view source;
    std::size_t at = 0;
    std::size_t line = 1;

    void step() {
        if (source[at++] == '\n') ++line;
    }

    bool longString() {
        if (source[at] != '[') return false;
        auto end = at + 1;
        while (end < source.size() && source[end] == '=') ++end;
        if (end == source.size() || source[end] != '[') return false;
        const auto closing = "]" + std::string(end - at - 1, '=') + "]";
        const auto close = source.find(closing, end + 1);
        if (close == std::string_view::npos) throw std::runtime_error("unterminated long string/comment");
        while (at < close + closing.size()) step();
        return true;
    }

public:
    explicit Lexer(std::string_view text) : source(text) {}

    Token next() {
        //wth is this
        while (at < source.size()) {
            if (std::isspace(static_cast<unsigned char>(source[at]))) {
                step();
                continue;
            }
            if (source.substr(at, 2) != "--") break;
            at += 2;
            if (at < source.size() && longString()) continue;
            while (at < source.size() && source[at] != '\n') step();
        }
        if (at == source.size()) return {"", Token::Symbol, line};
        const auto begin = at;
        const auto startLine = line;
        auto kind = Token::Symbol;
        const auto c = source[at];
        if (c == '\'' || c == '"') {
            kind = Token::String;
            step();
            bool closed = false;
            while (at < source.size()) {
                const auto ch = source[at];
                step();
                if (ch == '\\' && at < source.size()) {
                    step();
                } else if (ch == c) {
                    closed = true;
                    break;
                }
            }
            if (!closed) throw std::runtime_error("unterminated string");
        } else if (c == '[' && longString()) kind = Token::String;
        else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            kind = Token::Word;
            do {
                step();
            } while (at < source.size() &&
                     (std::isalnum(static_cast<unsigned char>(source[at])) || source[at] == '_'));
        } else if (std::isdigit(static_cast<unsigned char>(c)) ||
                   (c == '.' && at + 1 < source.size() && std::isdigit(static_cast<unsigned char>(source[at + 1])))) {
            kind = Token::Number;
            do {
                step();
            } while (at < source.size() &&
                     (std::isalnum(static_cast<unsigned char>(source[at])) || source[at] == '.' ||
                      ((source[at] == '+' || source[at] == '-') &&
                       (source[at - 1] == 'e' || source[at - 1] == 'E' || source[at - 1] == 'p' ||
                        source[at - 1] == 'P'))));
        } else {
            step();
        }
        return {std::string(source.substr(begin, at - begin)), kind, startLine};
    }
};
}  // namespace

bool CompatibleScriptProperty(const StateValue& a, const StateValue& b) {
    return (Numeric(a) && Numeric(b)) || (a.value.index() == b.value.index());
}

bool ValidateScriptProperties(const ScriptPropertyMap& values, std::string& error) {
    for (const auto& [name, value] : values) {
        if (name.empty() || !(std::holds_alternative<std::string>(value.value) ||
            std::holds_alternative<bool>(value.value) || (Numeric(value) && std::isfinite(value.Number())))) {
            error = "Property '" + name + "' must be a string, boolean or finite number";
            return false;
        }
    }
    error.clear();
    return true;
}

bool ReadScriptProperties(std::string_view source, ScriptPropertyMap& defaults, std::string& error) {
    std::size_t currentLine = 1;
    try {
        Lexer lexer(source);
        ScriptPropertyMap result;
        int blocks = 0;
        int braces = 0;
        bool found = false;
        std::string previous;
        for (auto token = lexer.next(); !token.text.empty(); token = lexer.next()) {
            currentLine = token.line;
            if (token.kind == Token::Word && token.text == "Properties" && blocks == 0 && braces == 0 &&
                previous != "." && previous != ":") {
                auto next = lexer.next();
                if (next.text != "=") {
                    previous = token.text;
                    continue;
                }
                if (previous == "local" || found) throw std::runtime_error("declare Properties once, without local");
                found = true;
                if (lexer.next().text != "{") throw std::runtime_error("Properties must be a literal table");
                auto key = lexer.next();
                while (key.text != "}") {
                    currentLine = key.line;
                    if (key.kind != Token::Word) throw std::runtime_error("expected a property identifier");
                    if (lexer.next().text != "=") throw std::runtime_error("expected '=' after property name");
                    auto value = lexer.next();
                    std::string literal = value.text;
                    if (value.text == "-") {
                        value = lexer.next();
                        literal += value.text;
                    }
                    if (value.kind != Token::String && value.kind != Token::Number &&
                        value.text != "true" && value.text != "false")
                        throw std::runtime_error("property defaults must be literal strings, numbers or booleans");
                    // Only a validated single literal is compiled. No source statements, names,
                    // function calls, operators or gameplay callbacks reach this isolated state.
                    std::unique_ptr<lua_State, decltype(&lua_close)> lua(luaL_newstate(), lua_close);
                    if (!lua) throw std::runtime_error("cannot allocate property reader");
                    const auto chunk = "return " + literal;
                    if (luaL_loadbuffer(lua.get(), chunk.data(), chunk.size(), "property") || lua_pcall(lua.get(), 0, 1, 0))
                        throw std::runtime_error(lua_tostring(lua.get(), -1));
                    StateValue decoded;
                    if (lua_type(lua.get(), -1) == LUA_TSTRING) {
                        std::size_t size = 0;
                        const char* text = lua_tolstring(lua.get(), -1, &size);
                        decoded = std::string(text, size);
                    } else if (lua_type(lua.get(), -1) == LUA_TBOOLEAN) {
                        decoded = bool(lua_toboolean(lua.get(), -1));
                    } else {
                        decoded = double(lua_tonumber(lua.get(), -1));
                    }
                    if (!result.emplace(key.text, std::move(decoded)).second)
                        throw std::runtime_error("duplicate property: " + key.text);
                    key = lexer.next();
                    if (key.text == "," || key.text == ";") key = lexer.next();
                    else if (key.text != "}") throw std::runtime_error("expected ',' or '}' after literal default");
                }
            } else if (token.kind == Token::Word) {
                if (token.text == "function" || token.text == "then" || token.text == "do" ||
                    token.text == "repeat") {
                    ++blocks;
                } else if (token.text == "end" || token.text == "until" || token.text == "elseif") {
                    --blocks;
                }
            } else if (token.text == "{") {
                ++braces;
            } else if (token.text == "}") {
                --braces;
            }
            previous = token.text;
        }
        if (!ValidateScriptProperties(result, error)) return false;
        defaults = std::move(result);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = "Properties:" + std::to_string(currentLine) + ": " + exception.what();
        return false;
    }
}
}  // namespace engine
