#include "BoundedJson.h"

#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace utils {
namespace {

class JsonComplexityLimiter final
    : public nlohmann::json_sax<nlohmann::json> {
public:
    using number_integer_t = nlohmann::json::number_integer_t;
    using number_unsigned_t = nlohmann::json::number_unsigned_t;
    using number_float_t = nlohmann::json::number_float_t;
    using string_t = nlohmann::json::string_t;
    using binary_t = nlohmann::json::binary_t;

    explicit JsonComplexityLimiter(const JsonComplexityLimits& limits)
        : limits_(limits) {}

    bool null() override { return beginValue(); }
    bool boolean(bool) override { return beginValue(); }
    bool number_integer(number_integer_t) override { return beginValue(); }
    bool number_unsigned(number_unsigned_t) override { return beginValue(); }
    bool number_float(number_float_t, const string_t&) override {
        return beginValue();
    }

    bool string(string_t& value) override {
        return addString(value.size()) && beginValue();
    }

    bool binary(binary_t&) override {
        return fail("binary values are not accepted in JSON");
    }

    bool start_object(std::size_t elements) override {
        return startContainer(ContainerKind::Object, elements);
    }

    bool key(string_t& value) override {
        if (stack_.empty() || stack_.back().kind != ContainerKind::Object) {
            return fail("object key appeared outside an object");
        }
        if (!incrementContainer(stack_.back())) {
            return false;
        }
        return addString(value.size());
    }

    bool end_object() override {
        return endContainer(ContainerKind::Object);
    }

    bool start_array(std::size_t elements) override {
        return startContainer(ContainerKind::Array, elements);
    }

    bool end_array() override {
        return endContainer(ContainerKind::Array);
    }

    bool parse_error(std::size_t position,
                     const std::string&,
                     const nlohmann::detail::exception&) override {
        if (error_.empty()) {
            error_ = "invalid JSON syntax at byte " +
                     std::to_string(position);
        }
        return false;
    }

    const std::string& error() const { return error_; }

private:
    enum class ContainerKind { Object, Array };

    struct ContainerFrame {
        ContainerKind kind = ContainerKind::Object;
        std::size_t items = 0;
    };

    bool fail(std::string message) {
        if (error_.empty()) {
            error_ = std::move(message);
        }
        return false;
    }

    bool incrementContainer(ContainerFrame& frame) {
        if (frame.items >= limits_.maxContainerItems) {
            return fail("JSON container exceeds " +
                        std::to_string(limits_.maxContainerItems) +
                        " item limit");
        }
        ++frame.items;
        return true;
    }

    bool beginValue() {
        if (nodes_ >= limits_.maxNodes) {
            return fail("JSON document exceeds " +
                        std::to_string(limits_.maxNodes) +
                        " node limit");
        }
        ++nodes_;
        if (!stack_.empty() &&
            stack_.back().kind == ContainerKind::Array) {
            return incrementContainer(stack_.back());
        }
        return true;
    }

    bool addString(std::size_t bytes) {
        if (bytes > limits_.maxStringBytes) {
            return fail("JSON string exceeds " +
                        std::to_string(limits_.maxStringBytes) +
                        " byte limit");
        }
        if (totalStringBytes_ > limits_.maxTotalStringBytes ||
            bytes > limits_.maxTotalStringBytes - totalStringBytes_) {
            return fail("JSON strings exceed " +
                        std::to_string(limits_.maxTotalStringBytes) +
                        " byte cumulative limit");
        }
        totalStringBytes_ += bytes;
        return true;
    }

    bool startContainer(ContainerKind kind, std::size_t elements) {
        if (!beginValue()) {
            return false;
        }
        if (stack_.size() >= limits_.maxDepth) {
            return fail("JSON nesting exceeds " +
                        std::to_string(limits_.maxDepth) +
                        " level limit");
        }
        if (elements != (std::numeric_limits<std::size_t>::max)() &&
            elements > limits_.maxContainerItems) {
            return fail("JSON container exceeds " +
                        std::to_string(limits_.maxContainerItems) +
                        " item limit");
        }
        stack_.push_back(ContainerFrame{kind, 0});
        return true;
    }

    bool endContainer(ContainerKind kind) {
        if (stack_.empty() || stack_.back().kind != kind) {
            return fail("JSON container nesting is inconsistent");
        }
        stack_.pop_back();
        return true;
    }

    const JsonComplexityLimits& limits_;
    std::vector<ContainerFrame> stack_;
    std::size_t nodes_ = 0;
    std::size_t totalStringBytes_ = 0;
    std::string error_;
};

bool validLimits(const JsonComplexityLimits& limits) {
    return limits.maxBytes != 0 && limits.maxDepth != 0 &&
           limits.maxNodes != 0 && limits.maxContainerItems != 0 &&
           limits.maxStringBytes != 0 &&
           limits.maxTotalStringBytes != 0;
}

} // namespace

bool validateJsonComplexity(std::string_view serialized,
                            const JsonComplexityLimits& limits,
                            std::string& error) {
    error.clear();
    if (!validLimits(limits)) {
        error = "JSON complexity limits are invalid";
        return false;
    }
    if (serialized.size() > limits.maxBytes) {
        error = "JSON input exceeds " + std::to_string(limits.maxBytes) +
                " byte limit";
        return false;
    }

    JsonComplexityLimiter limiter(limits);
    try {
        const bool accepted = nlohmann::json::sax_parse(
            serialized.begin(), serialized.end(), &limiter,
            nlohmann::json::input_format_t::json, true, false);
        if (!accepted) {
            error = limiter.error().empty()
                ? "invalid JSON structure"
                : limiter.error();
            return false;
        }
    } catch (const std::bad_alloc&) {
        error = "JSON structure validation allocation failed";
        return false;
    } catch (const std::exception&) {
        error = "JSON structure validation failed";
        return false;
    }
    return true;
}

bool parseBoundedJson(std::string_view serialized,
                      const JsonComplexityLimits& limits,
                      nlohmann::json& document,
                      std::string& error) {
    if (!validateJsonComplexity(serialized, limits, error)) {
        return false;
    }

    try {
        nlohmann::json parsed = nlohmann::json::parse(
            serialized.begin(), serialized.end());
        document = std::move(parsed);
    } catch (const std::bad_alloc&) {
        error = "JSON DOM allocation failed";
        return false;
    } catch (const nlohmann::json::exception&) {
        error = "JSON DOM construction failed after structural validation";
        return false;
    } catch (const std::exception&) {
        error = "JSON DOM construction failed";
        return false;
    }
    return true;
}

} // namespace utils
