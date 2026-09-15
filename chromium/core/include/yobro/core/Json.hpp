#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace yobro::core {

class JsonError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct JsonParseOptions {
    std::size_t maxBytes = 1'048'576;
    std::size_t maxDepth = 64;
    bool rejectDuplicateKeys = true;
};

class Json final {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;

    enum class Type { null, boolean, integer, number, string, array, object };

    Json() noexcept;
    Json(std::nullptr_t) noexcept;
    Json(bool value) noexcept;
    Json(std::int64_t value) noexcept;
    Json(double value);
    Json(const char *value);
    Json(std::string value);
    Json(std::string_view value);
    Json(Array value);
    Json(Object value);

    [[nodiscard]] Type type() const noexcept;
    [[nodiscard]] bool isNull() const noexcept;
    [[nodiscard]] bool isBoolean() const noexcept;
    [[nodiscard]] bool isInteger() const noexcept;
    [[nodiscard]] bool isNumber() const noexcept;
    [[nodiscard]] bool isString() const noexcept;
    [[nodiscard]] bool isArray() const noexcept;
    [[nodiscard]] bool isObject() const noexcept;

    [[nodiscard]] bool asBoolean() const;
    [[nodiscard]] std::int64_t asInteger() const;
    [[nodiscard]] double asNumber() const;
    [[nodiscard]] const std::string &asString() const;
    [[nodiscard]] const Array &asArray() const;
    [[nodiscard]] Array &asArray();
    [[nodiscard]] const Object &asObject() const;
    [[nodiscard]] Object &asObject();

    [[nodiscard]] const Json *find(std::string_view key) const noexcept;
    [[nodiscard]] Json *find(std::string_view key) noexcept;

    [[nodiscard]] static Json parse(
        std::string_view source,
        JsonParseOptions options = {}
    );
    [[nodiscard]] std::string serialize() const;

    friend bool operator==(const Json &, const Json &) = default;

private:
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object>;
    Storage storage_;
};

} // namespace yobro::core
