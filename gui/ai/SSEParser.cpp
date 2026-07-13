#ifdef HAVE_AI_CHAT

#include "SSEParser.h"

#include "AiLimits.h"

#include <exception>
#include <utility>

namespace AI {

SSEParser::SSEParser(SSECallback callback)
    : callback_(std::move(callback)) {}

void SSEParser::feed(const char* data, size_t length) {
    if (failed_) {
        return;
    }
    for (size_t index = 0; index < length; ++index) {
        const char value = data[index];
        if (value == '\r') {
            continue;
        }
        if (value == '\n') {
            processLine(currentLine_);
            currentLine_.clear();
        } else {
            if (currentLine_.size() >= Limits::kMaxSseLineBytes) {
                fail("SSE line exceeds 1 MiB limit", true);
                return;
            }
            currentLine_.push_back(value);
        }
    }
}

void SSEParser::finish() {
    if (failed_) {
        return;
    }
    if (!currentLine_.empty()) {
        processLine(currentLine_);
        currentLine_.clear();
    }
    flushEvent();
}

void SSEParser::fail(std::string error, bool limitExceeded) {
    if (failed_) {
        return;
    }
    failed_ = true;
    limitExceeded_ = limitExceeded;
    error_ = std::move(error);
    currentLine_.clear();
    eventBuffer_.clear();
}

bool SSEParser::failed() const {
    return failed_;
}

bool SSEParser::limitExceeded() const {
    return limitExceeded_;
}

const std::string& SSEParser::error() const {
    return error_;
}

void SSEParser::processLine(const std::string& line) {
    if (failed_) {
        return;
    }
    if (line.empty()) {
        flushEvent();
        return;
    }
    if (line.front() == ':') {
        return;
    }
    if (line.compare(0, 5, "data:") != 0) {
        return;
    }

    size_t start = 5;
    if (start < line.size() && line[start] == ' ') {
        ++start;
    }
    const size_t separatorBytes = eventBuffer_.empty() ? 0u : 1u;
    const size_t fragmentBytes = line.size() - start;
    if (Limits::wouldExceed(eventBuffer_.size(), separatorBytes,
                            Limits::kMaxSseEventBytes) ||
        Limits::wouldExceed(eventBuffer_.size() + separatorBytes,
                            fragmentBytes, Limits::kMaxSseEventBytes)) {
        fail("SSE event exceeds 2 MiB limit", true);
        return;
    }
    if (!eventBuffer_.empty()) {
        eventBuffer_.push_back('\n');
    }
    eventBuffer_.append(line, start, std::string::npos);
}

void SSEParser::flushEvent() {
    if (eventBuffer_.empty()) {
        return;
    }
    if (callback_) {
        try {
            callback_(eventBuffer_);
        } catch (const std::exception& error) {
            fail(error.what());
        } catch (...) {
            fail("unknown callback exception");
        }
    }
    eventBuffer_.clear();
}

} // namespace AI

#endif // HAVE_AI_CHAT
