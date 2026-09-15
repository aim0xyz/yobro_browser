#include "yobro/core/Json.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>
#include <utility>

namespace yobro::core {
namespace {

[[noreturn]] void wrongType(const char *expected) {
    throw JsonError(std::string("JSON value is not ") + expected + ".");
}

void appendUtf8(std::string &output, std::uint32_t codePoint) {
    if (codePoint <= 0x7fU) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else if (codePoint <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (codePoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    }
}

std::size_t validatedUtf8Length(std::string_view value, std::size_t offset) {
    const auto first = static_cast<unsigned char>(value[offset]);
    std::size_t length = 0;
    if (first >= 0xc2U && first <= 0xdfU)
        length = 2;
    else if (first >= 0xe0U && first <= 0xefU)
        length = 3;
    else if (first >= 0xf0U && first <= 0xf4U)
        length = 4;
    else
        throw JsonError("Invalid UTF-8 in JSON string.");

    if (offset + length > value.size())
        throw JsonError("Truncated UTF-8 in JSON string.");
    for (std::size_t index = 1; index < length; ++index) {
        const auto continuation = static_cast<unsigned char>(value[offset + index]);
        if ((continuation & 0xc0U) != 0x80U)
            throw JsonError("Invalid UTF-8 continuation byte in JSON string.");
    }

    const auto second = static_cast<unsigned char>(value[offset + 1]);
    if ((first == 0xe0U && second < 0xa0U)
        || (first == 0xedU && second > 0x9fU)
        || (first == 0xf0U && second < 0x90U)
        || (first == 0xf4U && second > 0x8fU)) {
        throw JsonError("Non-scalar or overlong UTF-8 in JSON string.");
    }
    return length;
}

class Parser final {
public:
    Parser(std::string_view source, JsonParseOptions options)
        : source_(source), options_(options) {}

    Json parseDocument() {
        if (source_.size() > options_.maxBytes)
            fail("JSON input exceeds the configured byte limit");
        skipWhitespace();
        Json value = parseValue(0);
        skipWhitespace();
        if (position_ != source_.size())
            fail("Trailing content after JSON value");
        return value;
    }

private:
    [[noreturn]] void fail(const char *message) const {
        throw JsonError(std::string(message) + " at byte " + std::to_string(position_) + ".");
    }

    void skipWhitespace() {
        while (position_ < source_.size()) {
            const char value = source_[position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n')
                break;
            ++position_;
        }
    }

    bool consume(char expected) {
        if (position_ >= source_.size() || source_[position_] != expected)
            return false;
        ++position_;
        return true;
    }

    Json parseValue(std::size_t depth) {
        if (position_ >= source_.size())
            fail("Expected a JSON value");
        switch (source_[position_]) {
        case 'n': return parseLiteral("null", Json(nullptr));
        case 't': return parseLiteral("true", Json(true));
        case 'f': return parseLiteral("false", Json(false));
        case '"': return Json(parseString());
        case '[': return parseArray(depth);
        case '{': return parseObject(depth);
        default:
            if (source_[position_] == '-' || (source_[position_] >= '0' && source_[position_] <= '9'))
                return parseNumber();
            fail("Expected a JSON value");
        }
    }

    Json parseLiteral(std::string_view literal, Json value) {
        if (source_.substr(position_, literal.size()) != literal)
            fail("Invalid JSON literal");
        position_ += literal.size();
        return value;
    }

    std::uint16_t parseHexQuad() {
        if (position_ + 4 > source_.size())
            fail("Truncated Unicode escape");
        std::uint16_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const char digit = source_[position_++];
            value = static_cast<std::uint16_t>(value << 4U);
            if (digit >= '0' && digit <= '9')
                value = static_cast<std::uint16_t>(value | static_cast<unsigned>(digit - '0'));
            else if (digit >= 'a' && digit <= 'f')
                value = static_cast<std::uint16_t>(value | static_cast<unsigned>(digit - 'a' + 10));
            else if (digit >= 'A' && digit <= 'F')
                value = static_cast<std::uint16_t>(value | static_cast<unsigned>(digit - 'A' + 10));
            else
                fail("Invalid Unicode escape");
        }
        return value;
    }

    std::string parseString() {
        if (!consume('"'))
            fail("Expected a JSON string");
        std::string result;
        while (position_ < source_.size()) {
            const auto byte = static_cast<unsigned char>(source_[position_++]);
            if (byte == '"')
                return result;
            if (byte < 0x20U)
                fail("Unescaped control byte in JSON string");
            if (byte == '\\') {
                if (position_ >= source_.size())
                    fail("Truncated JSON escape");
                const char escape = source_[position_++];
                switch (escape) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    const std::uint16_t first = parseHexQuad();
                    std::uint32_t codePoint = first;
                    if (first >= 0xd800U && first <= 0xdbffU) {
                        if (!consume('\\') || !consume('u'))
                            fail("High surrogate is missing its low surrogate");
                        const std::uint16_t second = parseHexQuad();
                        if (second < 0xdc00U || second > 0xdfffU)
                            fail("Invalid low surrogate");
                        codePoint = 0x10000U
                            + ((static_cast<std::uint32_t>(first) - 0xd800U) << 10U)
                            + (static_cast<std::uint32_t>(second) - 0xdc00U);
                    } else if (first >= 0xdc00U && first <= 0xdfffU) {
                        fail("Unexpected low surrogate");
                    }
                    appendUtf8(result, codePoint);
                    break;
                }
                default: fail("Invalid JSON escape");
                }
                continue;
            }
            if (byte < 0x80U) {
                result.push_back(static_cast<char>(byte));
                continue;
            }
            --position_;
            const std::size_t length = validatedUtf8Length(source_, position_);
            result.append(source_.substr(position_, length));
            position_ += length;
        }
        fail("Unterminated JSON string");
    }

    Json parseArray(std::size_t depth) {
        if (depth >= options_.maxDepth)
            fail("JSON nesting exceeds the configured depth limit");
        consume('[');
        skipWhitespace();
        Json::Array result;
        if (consume(']'))
            return Json(std::move(result));
        while (true) {
            result.push_back(parseValue(depth + 1));
            skipWhitespace();
            if (consume(']'))
                return Json(std::move(result));
            if (!consume(','))
                fail("Expected ',' or ']' in JSON array");
            skipWhitespace();
        }
    }

    Json parseObject(std::size_t depth) {
        if (depth >= options_.maxDepth)
            fail("JSON nesting exceeds the configured depth limit");
        consume('{');
        skipWhitespace();
        Json::Object result;
        if (consume('}'))
            return Json(std::move(result));
        while (true) {
            if (position_ >= source_.size() || source_[position_] != '"')
                fail("Expected a string key in JSON object");
            std::string key = parseString();
            skipWhitespace();
            if (!consume(':'))
                fail("Expected ':' after JSON object key");
            skipWhitespace();
            Json value = parseValue(depth + 1);
            const auto [iterator, inserted] = result.emplace(std::move(key), std::move(value));
            (void)iterator;
            if (!inserted && options_.rejectDuplicateKeys)
                fail("Duplicate JSON object key");
            skipWhitespace();
            if (consume('}'))
                return Json(std::move(result));
            if (!consume(','))
                fail("Expected ',' or '}' in JSON object");
            skipWhitespace();
        }
    }

    Json parseNumber() {
        const std::size_t start = position_;
        consume('-');
        if (consume('0')) {
            if (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9')
                fail("Leading zero in JSON number");
        } else {
            if (position_ >= source_.size() || source_[position_] < '1' || source_[position_] > '9')
                fail("Invalid JSON number");
            while (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9')
                ++position_;
        }

        bool integral = true;
        if (consume('.')) {
            integral = false;
            if (position_ >= source_.size() || source_[position_] < '0' || source_[position_] > '9')
                fail("Missing fraction digits in JSON number");
            while (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9')
                ++position_;
        }
        if (position_ < source_.size() && (source_[position_] == 'e' || source_[position_] == 'E')) {
            integral = false;
            ++position_;
            if (position_ < source_.size() && (source_[position_] == '+' || source_[position_] == '-'))
                ++position_;
            if (position_ >= source_.size() || source_[position_] < '0' || source_[position_] > '9')
                fail("Missing exponent digits in JSON number");
            while (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9')
                ++position_;
        }

        const std::string_view encoded = source_.substr(start, position_ - start);
        if (integral) {
            std::int64_t integer = 0;
            const auto converted = std::from_chars(encoded.data(), encoded.data() + encoded.size(), integer);
            if (converted.ec == std::errc() && converted.ptr == encoded.data() + encoded.size())
                return Json(integer);
        }
        double number = 0.0;
        const auto converted = std::from_chars(
            encoded.data(),
            encoded.data() + encoded.size(),
            number,
            std::chars_format::general
        );
        if (converted.ec != std::errc() || converted.ptr != encoded.data() + encoded.size() || !std::isfinite(number))
            fail("JSON number is outside the supported finite range");
        return Json(number);
    }

    std::string_view source_;
    JsonParseOptions options_;
    std::size_t position_ = 0;
};

void appendEscaped(std::string &output, std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (std::size_t position = 0; position < value.size();) {
        const auto byte = static_cast<unsigned char>(value[position]);
        switch (byte) {
        case '"': output += "\\\""; ++position; continue;
        case '\\': output += "\\\\"; ++position; continue;
        case '\b': output += "\\b"; ++position; continue;
        case '\f': output += "\\f"; ++position; continue;
        case '\n': output += "\\n"; ++position; continue;
        case '\r': output += "\\r"; ++position; continue;
        case '\t': output += "\\t"; ++position; continue;
        default: break;
        }
        if (byte < 0x20U) {
            output += "\\u00";
            output.push_back(hex[(byte >> 4U) & 0x0fU]);
            output.push_back(hex[byte & 0x0fU]);
            ++position;
        } else if (byte < 0x80U) {
            output.push_back(static_cast<char>(byte));
            ++position;
        } else {
            const std::size_t length = validatedUtf8Length(value, position);
            output.append(value.substr(position, length));
            position += length;
        }
    }
    output.push_back('"');
}

void appendSerialized(std::string &output, const Json &value) {
    switch (value.type()) {
    case Json::Type::null:
        output += "null";
        break;
    case Json::Type::boolean:
        output += value.asBoolean() ? "true" : "false";
        break;
    case Json::Type::integer: {
        char buffer[32];
        const auto encoded = std::to_chars(buffer, buffer + sizeof(buffer), value.asInteger());
        if (encoded.ec != std::errc())
            throw JsonError("Could not serialize JSON integer.");
        output.append(buffer, encoded.ptr);
        break;
    }
    case Json::Type::number: {
        char buffer[64];
        const auto encoded = std::to_chars(
            buffer,
            buffer + sizeof(buffer),
            value.asNumber(),
            std::chars_format::general,
            std::numeric_limits<double>::max_digits10
        );
        if (encoded.ec != std::errc())
            throw JsonError("Could not serialize JSON number.");
        output.append(buffer, encoded.ptr);
        break;
    }
    case Json::Type::string:
        appendEscaped(output, value.asString());
        break;
    case Json::Type::array: {
        output.push_back('[');
        bool first = true;
        for (const Json &item : value.asArray()) {
            if (!first)
                output.push_back(',');
            first = false;
            appendSerialized(output, item);
        }
        output.push_back(']');
        break;
    }
    case Json::Type::object: {
        output.push_back('{');
        bool first = true;
        for (const auto &[key, item] : value.asObject()) {
            if (!first)
                output.push_back(',');
            first = false;
            appendEscaped(output, key);
            output.push_back(':');
            appendSerialized(output, item);
        }
        output.push_back('}');
        break;
    }
    }
}

} // namespace

Json::Json() noexcept : storage_(nullptr) {}
Json::Json(std::nullptr_t) noexcept : storage_(nullptr) {}
Json::Json(bool value) noexcept : storage_(value) {}
Json::Json(std::int64_t value) noexcept : storage_(value) {}
Json::Json(double value) : storage_(value) {
    if (!std::isfinite(value))
        throw JsonError("JSON numbers must be finite.");
}
Json::Json(const char *value) : Json(std::string(value ? value : "")) {}
Json::Json(std::string value) : storage_(std::move(value)) {}
Json::Json(std::string_view value) : Json(std::string(value)) {}
Json::Json(Array value) : storage_(std::move(value)) {}
Json::Json(Object value) : storage_(std::move(value)) {}

Json::Type Json::type() const noexcept { return static_cast<Type>(storage_.index()); }
bool Json::isNull() const noexcept { return std::holds_alternative<std::nullptr_t>(storage_); }
bool Json::isBoolean() const noexcept { return std::holds_alternative<bool>(storage_); }
bool Json::isInteger() const noexcept { return std::holds_alternative<std::int64_t>(storage_); }
bool Json::isNumber() const noexcept { return isInteger() || std::holds_alternative<double>(storage_); }
bool Json::isString() const noexcept { return std::holds_alternative<std::string>(storage_); }
bool Json::isArray() const noexcept { return std::holds_alternative<Array>(storage_); }
bool Json::isObject() const noexcept { return std::holds_alternative<Object>(storage_); }

bool Json::asBoolean() const {
    if (!isBoolean()) wrongType("a boolean");
    return std::get<bool>(storage_);
}
std::int64_t Json::asInteger() const {
    if (!isInteger()) wrongType("an integer");
    return std::get<std::int64_t>(storage_);
}
double Json::asNumber() const {
    if (isInteger()) return static_cast<double>(std::get<std::int64_t>(storage_));
    if (!std::holds_alternative<double>(storage_)) wrongType("a number");
    return std::get<double>(storage_);
}
const std::string &Json::asString() const {
    if (!isString()) wrongType("a string");
    return std::get<std::string>(storage_);
}
const Json::Array &Json::asArray() const {
    if (!isArray()) wrongType("an array");
    return std::get<Array>(storage_);
}
Json::Array &Json::asArray() {
    if (!isArray()) wrongType("an array");
    return std::get<Array>(storage_);
}
const Json::Object &Json::asObject() const {
    if (!isObject()) wrongType("an object");
    return std::get<Object>(storage_);
}
Json::Object &Json::asObject() {
    if (!isObject()) wrongType("an object");
    return std::get<Object>(storage_);
}

const Json *Json::find(std::string_view key) const noexcept {
    if (!isObject()) return nullptr;
    const auto &object = std::get<Object>(storage_);
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}
Json *Json::find(std::string_view key) noexcept {
    if (!isObject()) return nullptr;
    auto &object = std::get<Object>(storage_);
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

Json Json::parse(std::string_view source, JsonParseOptions options) {
    return Parser(source, options).parseDocument();
}

std::string Json::serialize() const {
    std::string result;
    appendSerialized(result, *this);
    return result;
}

} // namespace yobro::core
