#pragma once

#include "nlohmann/json.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace utils {

struct JsonComplexityLimits {
    std::size_t maxBytes = 0;
    std::size_t maxDepth = 0;
    std::size_t maxNodes = 0;
    std::size_t maxContainerItems = 0;
    std::size_t maxStringBytes = 0;
    std::size_t maxTotalStringBytes = 0;
};

bool validateJsonComplexity(std::string_view serialized,
                            const JsonComplexityLimits& limits,
                            std::string& error);

bool parseBoundedJson(std::string_view serialized,
                      const JsonComplexityLimits& limits,
                      nlohmann::json& document,
                      std::string& error);

} // namespace utils
