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
    void fail(std::string error, bool limitExceeded = false);

    bool failed() const;
    bool limitExceeded() const;
    const std::string& error() const;

private:
    void processLine(const std::string& line);
    void flushEvent();

    SSECallback callback_;
    std::string currentLine_;
    std::string eventBuffer_;
    bool failed_ = false;
    bool limitExceeded_ = false;
    std::string error_;
};

} // namespace AI

#endif // HAVE_AI_CHAT
