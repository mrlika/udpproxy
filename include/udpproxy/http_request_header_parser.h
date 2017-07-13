#pragma once

#include <experimental/string_view>
#include <functional>

namespace UdpProxy {

template <typename Iterator>
class HttpRequestHeaderParser {
public:
    explicit HttpRequestHeaderParser(size_t maxHeaderSize) noexcept
            : maxHeaderSize(maxHeaderSize),
              currentMatcher(std::bind(&HttpRequestHeaderParser::methodMatcher, this, std::placeholders::_1, std::placeholders::_2)) {
    }

    HttpRequestHeaderParser(const HttpRequestHeaderParser&) = delete;
    HttpRequestHeaderParser(HttpRequestHeaderParser&&) = delete;

    std::pair<Iterator, bool> operator()(Iterator begin, Iterator end) noexcept {
        return currentMatcher(begin, end);
    }

    bool isSucceeded() { return success; }
    std::experimental::string_view getMethod() const noexcept { return {methodBegin, static_cast<size_t>(methodEnd - methodBegin)}; }
    std::experimental::string_view getUri() const noexcept { return {uriBegin, static_cast<size_t>(uriEnd - uriBegin)}; }
    std::experimental::string_view getProtocolVersion() const noexcept { return {protocolVersionBegin, static_cast<size_t>(protocolVersionEnd - protocolVersionBegin)}; }
    std::experimental::string_view getHeaderFields() const noexcept { return {headerFieldsBegin, static_cast<size_t>(headerFieldsEnd - headerFieldsBegin)}; }

private:
    std::pair<Iterator, bool> methodMatcher(Iterator begin, Iterator end) noexcept {
        if (methodBegin == nullptr) {
            methodBegin = &*begin;
        }

        Iterator i = begin;

        while (i != end) {
            if (bytesRead++ == maxHeaderSize) {
                return std::make_pair(i, true);
            }

            char c = *i;

            // TODO: validate METHOD chars

            if (c == ' ') {
                methodEnd =  &*i;
                currentMatcher = std::bind(&HttpRequestHeaderParser::uriMatcher, this, std::placeholders::_1, std::placeholders::_2);
                i++;
                return uriMatcher(i, end);
            }

            i++;
        }

        return std::make_pair(i, false);
    }

    std::pair<Iterator, bool> uriMatcher(Iterator begin, Iterator end) noexcept {
        if (uriBegin == nullptr) {
            uriBegin = &*begin;
        }

        Iterator i = begin;

        while (i != end) {
            if (bytesRead++ == maxHeaderSize) {
                return std::make_pair(i, true);
            }

            char c = *i;

            // TODO: validate URI chars

            if (c == ' ') {
                uriEnd =  &*i;
                currentMatcher = std::bind(&HttpRequestHeaderParser::protocolVersionMatcher, this, std::placeholders::_1, std::placeholders::_2);
                i++;
                return protocolVersionMatcher(i, end);
            }

            i++;
        }

        return std::make_pair(i, false);
    }

    std::pair<Iterator, bool> protocolVersionMatcher(Iterator begin, Iterator end) noexcept {
        if (protocolVersionBegin == nullptr) {
            protocolVersionBegin = &*begin;
        }

        Iterator i = begin;

        while (i != end) {
            if (bytesRead++ == maxHeaderSize) {
                return std::make_pair(i, true);
            }

            char c = *i;

            // TODO: validate HTTP version chars

            if ((previousChar == '\r') && (c == '\n')) {
                protocolVersionEnd =  &*(i-1);
                currentMatcher = std::bind(&HttpRequestHeaderParser::headerFieldsMatcher, this, std::placeholders::_1, std::placeholders::_2);
                previousChar ='\n';
                i++;
                return headerFieldsMatcher(i, end);
            }

            previousChar = c;
            i++;
        }

        return std::make_pair(i, false);
    }

    std::pair<Iterator, bool> headerFieldsMatcher(Iterator begin, Iterator end) noexcept {
        if (headerFieldsBegin == nullptr) {
            headerFieldsBegin = &*begin;
        }

        Iterator i = begin;

        while (i != end) {
            if (bytesRead++ == maxHeaderSize) {
                return std::make_pair(i, true);
            }

            char c = *i;

            // TODO: split and validate header fields

            if ((previousChar == '\r') && (c == '\n')) {
                if (newLine) {
                    headerFieldsEnd =  &*(i-1);
                    success = true;
                    i++;
                    return std::make_pair(i, true);
                } else {
                    newLine = true;
                }
            } else if ((newLine != true) || (previousChar != '\n') || (c != '\r')) {
                newLine = false;
            }

            previousChar = c;
            i++;
        }

        return std::make_pair(i, false);
    }

    size_t maxHeaderSize;
    std::function<std::pair<Iterator, bool>(Iterator begin, Iterator end) noexcept> currentMatcher;
    size_t bytesRead = 0;
    char previousChar = 0;
    bool newLine = true;
    bool success = false;

    typename std::iterator_traits<Iterator>::pointer methodBegin = nullptr;
    typename std::iterator_traits<Iterator>::pointer methodEnd = nullptr;
    typename std::iterator_traits<Iterator>::pointer uriBegin = nullptr;
    typename std::iterator_traits<Iterator>::pointer uriEnd = nullptr;
    typename std::iterator_traits<Iterator>::pointer protocolVersionBegin = nullptr;
    typename std::iterator_traits<Iterator>::pointer protocolVersionEnd = nullptr;
    typename std::iterator_traits<Iterator>::pointer headerFieldsBegin = nullptr;
    typename std::iterator_traits<Iterator>::pointer headerFieldsEnd = nullptr;
};

}
