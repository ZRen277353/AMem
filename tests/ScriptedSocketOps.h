#pragma once

#include "socket/client.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace TestSupport {

class ScriptedSocketOps final : public IWindowsSocketOps {
public:
    struct Step {
        enum class Kind {
            Transfer,
            Error,
            Eof,
        };

        Kind kind = Kind::Transfer;
        int count = 0;
        int error = 0;
        std::vector<char> data;
    };

    struct Endpoint {
        bool connected = false;
        bool closed = false;
        unsigned closeCalls = 0;
        std::deque<Step> sendSteps;
        std::deque<Step> receiveSteps;
        std::vector<char> sent;
        std::vector<int> sendRequestLengths;
        std::vector<int> receiveRequestLengths;
    };

    SOCKET CreateTcpSocket() override {
        const SOCKET value = nextSocket_++;
        endpoints_.emplace(value, Endpoint{});
        lastCreated_ = value;
        return value;
    }

    int Connect(SOCKET socketValue, const sockaddr*, int) override {
        Endpoint& value = endpoint(socketValue);
        if (!connectErrors_.empty()) {
            lastError_ = connectErrors_.front();
            connectErrors_.pop_front();
            return SOCKET_ERROR;
        }
        value.connected = true;
        return 0;
    }

    int Send(SOCKET socketValue, const char* buffer, int length,
             int) override {
        Endpoint& value = endpoint(socketValue);
        value.sendRequestLengths.push_back(length);
        if (value.sendSteps.empty()) {
            lastError_ = WSAEINVAL;
            return SOCKET_ERROR;
        }

        Step step = std::move(value.sendSteps.front());
        value.sendSteps.pop_front();
        if (step.kind == Step::Kind::Error) {
            lastError_ = step.error;
            return SOCKET_ERROR;
        }
        if (step.kind == Step::Kind::Eof) {
            return 0;
        }

        const int transferred = (std::min)(step.count, length);
        value.sent.insert(value.sent.end(), buffer, buffer + transferred);
        return transferred;
    }

    int Receive(SOCKET socketValue, char* buffer, int length,
                int) override {
        Endpoint& value = endpoint(socketValue);
        value.receiveRequestLengths.push_back(length);
        if (value.receiveSteps.empty()) {
            lastError_ = WSAEINVAL;
            return SOCKET_ERROR;
        }

        Step step = std::move(value.receiveSteps.front());
        value.receiveSteps.pop_front();
        if (step.kind == Step::Kind::Error) {
            lastError_ = step.error;
            return SOCKET_ERROR;
        }
        if (step.kind == Step::Kind::Eof) {
            return 0;
        }

        const int transferred = (std::min)(
            length, static_cast<int>(step.data.size()));
        std::memcpy(buffer, step.data.data(),
                    static_cast<size_t>(transferred));
        return transferred;
    }

    int Close(SOCKET socketValue) override {
        Endpoint& value = endpoint(socketValue);
        value.connected = false;
        value.closed = true;
        ++value.closeCalls;
        return 0;
    }

    int LastError() const override {
        return lastError_;
    }

    SOCKET lastCreated() const {
        return lastCreated_;
    }

    size_t createdCount() const {
        return endpoints_.size();
    }

    Endpoint& endpoint(SOCKET socketValue) {
        const auto found = endpoints_.find(socketValue);
        if (found == endpoints_.end()) {
            throw std::runtime_error("unknown scripted socket");
        }
        return found->second;
    }

    const Endpoint& endpoint(SOCKET socketValue) const {
        const auto found = endpoints_.find(socketValue);
        if (found == endpoints_.end()) {
            throw std::runtime_error("unknown scripted socket");
        }
        return found->second;
    }

    void failNextConnect(int error) {
        connectErrors_.push_back(error);
    }

    void queueSendCount(SOCKET socketValue, int count) {
        endpoint(socketValue).sendSteps.push_back(
            Step{Step::Kind::Transfer, count, 0, {}});
    }

    void queueSendError(SOCKET socketValue, int error) {
        endpoint(socketValue).sendSteps.push_back(
            Step{Step::Kind::Error, 0, error, {}});
    }

    void queueReceive(SOCKET socketValue, std::string data) {
        endpoint(socketValue).receiveSteps.push_back(
            Step{Step::Kind::Transfer, 0, 0,
                 std::vector<char>(data.begin(), data.end())});
    }

    void queueReceiveError(SOCKET socketValue, int error) {
        endpoint(socketValue).receiveSteps.push_back(
            Step{Step::Kind::Error, 0, error, {}});
    }

    void queueReceiveEof(SOCKET socketValue) {
        endpoint(socketValue).receiveSteps.push_back(
            Step{Step::Kind::Eof, 0, 0, {}});
    }

private:
    std::unordered_map<SOCKET, Endpoint> endpoints_;
    std::deque<int> connectErrors_;
    SOCKET nextSocket_ = 100;
    SOCKET lastCreated_ = INVALID_SOCKET;
    int lastError_ = 0;
};

} // namespace TestSupport
