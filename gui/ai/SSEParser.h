#pragma once
#ifdef HAVE_AI_CHAT

#include "HttpClient.h"

#include <cstddef>
#include <string>

namespace AI {

class SSEParser {
public:
    explicit SSEParser(SSECallback callback);

    void feed(const char* data, size_t length);
    void finish();

    bool callbackFailed() const;
    const std::string& callbackError() const;

private:
    void processLine(const std::string& line);
    void flushEvent();

    SSECallback callback_;
    std::string currentLine_;
    std::string eventBuffer_;
    bool callbackFailed_ = false;
    std::string callbackError_;
};

} // namespace AI

#endif // HAVE_AI_CHAT
