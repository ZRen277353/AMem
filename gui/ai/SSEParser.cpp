#ifdef HAVE_AI_CHAT

#include "SSEParser.h"

#include <exception>
#include <utility>

namespace AI {

SSEParser::SSEParser(SSECallback callback)
    : callback_(std::move(callback)) {}

void SSEParser::feed(const char* data, size_t length) {
    if (callbackFailed_) {
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
            currentLine_.push_back(value);
        }
    }
}

void SSEParser::finish() {
    if (callbackFailed_) {
        return;
    }
    if (!currentLine_.empty()) {
        processLine(currentLine_);
        currentLine_.clear();
    }
    flushEvent();
}

bool SSEParser::callbackFailed() const {
    return callbackFailed_;
}

const std::string& SSEParser::callbackError() const {
    return callbackError_;
}

void SSEParser::processLine(const std::string& line) {
    if (callbackFailed_) {
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
            callbackFailed_ = true;
            callbackError_ = error.what();
        } catch (...) {
            callbackFailed_ = true;
            callbackError_ = "unknown callback exception";
        }
    }
    eventBuffer_.clear();
}

} // namespace AI

#endif // HAVE_AI_CHAT
