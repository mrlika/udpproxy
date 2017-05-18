#pragma once

#include <boost/asio.hpp>
#include <boost/asio/system_timer.hpp>

#include <iostream>
#include <list>
#include <streambuf>

#include "version.h"

namespace UdpProxy {

using boost::asio::ip::tcp;
using namespace std::chrono_literals;

template <typename Allocator, bool SendHttpResponses>
class SimpleHttpServer {
public:
    class HttpRequest {
    public:
        explicit HttpRequest(const std::shared_ptr<typename SimpleHttpServer::HttpClient> &httpClient) noexcept
            : httpClient(httpClient), buffer(httpClient->buffer), method(httpClient->httpMethod), uri(httpClient->httpUri),
              protocolVersion(httpClient->protocolVersion), headerFields(httpClient->httpHeaderFields) {
        }

        HttpRequest(const HttpRequest&) = default;
        HttpRequest(HttpRequest&&) = default;

        ~HttpRequest() noexcept {
            if (!httpClient.expired()) {
                httpClient.lock()->removeFromServer();
            }
        }

        std::experimental::string_view getMethod() const noexcept { return method; }
        std::experimental::string_view getUri() const noexcept { return uri; }
        std::experimental::string_view getProtocolVersion() const noexcept { return protocolVersion; }
        std::experimental::string_view getHeaderFields() const noexcept { return headerFields; }
        std::weak_ptr<tcp::socket> getSocket() const { return httpClient.expired() ? std::weak_ptr<tcp::socket>() : std::weak_ptr<tcp::socket>(httpClient.lock()->socket); }

        void cancelTimeout() const {
            if (!httpClient.expired()) {
                httpClient.lock()->timeoutTimer.cancel();
            }
        }

    private:
        std::weak_ptr<typename SimpleHttpServer::HttpClient> httpClient;
        std::shared_ptr<boost::asio::basic_streambuf<Allocator>> buffer;
        std::experimental::string_view method;
        std::experimental::string_view uri;
        std::experimental::string_view protocolVersion;
        std::experimental::string_view headerFields;
    };

    typedef std::function<void(std::shared_ptr<HttpRequest>) noexcept> RequestHandler;

    SimpleHttpServer(boost::asio::io_service &ioService, const tcp::endpoint &endpoint) : acceptor(ioService, endpoint) {}

    void runAsync() {
        startAccept();
    }

    void setMaxHttpHeaderSize(size_t value) noexcept { maxHttpHeaderSize = value; }
    size_t getMaxHttpHeaderSize() const noexcept { return maxHttpHeaderSize; }
    void setHttpConnectionTimeout(boost::asio::system_timer::duration value) noexcept { httpConnectionTimeout = value; }
    boost::asio::system_timer::duration getHttpConnectionTimeout() const noexcept { return httpConnectionTimeout; }
    void setMaxHttpClients(size_t value) noexcept { maxHttpClients = value; }
    size_t getMaxHttpClients() const noexcept { return maxHttpClients; }
    void setVerboseLogging(bool value) noexcept { verboseLogging = value; }
    bool getVerboseLogging() const noexcept { return verboseLogging; }
    void setRequestHandler(RequestHandler handler) { requestHandler = handler; }
    RequestHandler getRequestHandler() { return requestHandler; }

private:
    typedef boost::asio::buffers_iterator<boost::asio::streambuf::const_buffers_type> UntilIterator;
    typedef std::function<std::pair<UntilIterator, bool>(UntilIterator begin, UntilIterator end) noexcept> UntilFunction;

    class ServerError : public std::runtime_error {
    private:
        std::experimental::string_view httpResponse;

    public:
        explicit ServerError(const std::string& message, std::experimental::string_view httpResponse)
                : std::runtime_error(message), httpResponse(SendHttpResponses ? httpResponse : std::experimental::string_view()) {
        }

        explicit ServerError(const char *message, std::experimental::string_view httpResponse)
                : std::runtime_error(message), httpResponse(SendHttpResponses ? httpResponse : std::experimental::string_view()) {
        }

        std::experimental::string_view http_response() const noexcept {
            if (SendHttpResponses) {
                return httpResponse;
            } else {
                return {};
            }
        }
    };

    void startAccept() {
        auto socket = std::make_shared<tcp::socket>(acceptor.get_io_service());

        acceptor.async_accept(*socket, [this, socket = socket] (const boost::system::error_code &e) mutable {
            if (e) {
                if (e == boost::system::errc::operation_canceled) {
                    return;
                }

                std::cerr << "TCP accept error: " << e.message() << std::endl;
                return; // FIXME: is it good to stop accept loop?
            }

            auto client = std::make_shared<HttpClient>(socket, *this);
            httpClients.emplace_back(client);
            client->startCancelTimer();

            if ((maxHttpClients == 0) || (httpClients.size() <= maxHttpClients)) {
                client->validateHttpHeader();
            } else {
                if (verboseLogging) {
                    std::cerr << "Maximum of HTTP clients reached. Connection refused: " << socket->remote_endpoint() << std::endl;
                }

                client->sendFinalHttpResponse(
                    "HTTP/1.1 429 Too Many Requests\r\n"
                    "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                    "Content-Type: text/plain\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "Maximum number of clients reached");
            }

            startAccept();
        });
    }

    struct HttpClient : public std::enable_shared_from_this<HttpClient> {
        HttpClient(const std::shared_ptr<tcp::socket> &socket, SimpleHttpServer &server)
                : server(server), socket(socket), buffer(std::make_shared<boost::asio::basic_streambuf<Allocator>>(server.maxHttpHeaderSize)),
                  timeoutTimer(socket->get_io_service()), matchHttpHeader((server.maxHttpHeaderSize)) {
            buffer->prepare(server.maxHttpHeaderSize);
            if (server.verboseLogging) {
                std::cerr << "new connection " << socket->remote_endpoint() << std::endl;
            }
        }

        ~HttpClient() noexcept {
            if (server.verboseLogging) {
                std::cerr << "remove connection " << socket->remote_endpoint() << std::endl;
            }
        }

        void startCancelTimer() {
            if (server.httpConnectionTimeout == 0s) {
                return;
            }

            timeoutTimer.expires_from_now(server.httpConnectionTimeout);
            timeoutTimer.async_wait([this, reference = std::weak_ptr<HttpClient>(this->shared_from_this())] (const boost::system::error_code &e) {
                if (reference.expired()) {
                    return;
                }

                if (e != boost::system::errc::operation_canceled) {
                    removeFromServer();
                }
            });
        }

        void removeFromServer() {
            auto it = find_if(server.httpClients.begin(), server.httpClients.end(), [this] (const std::shared_ptr<HttpClient>& client) { return client.get() == this; });
            assert(it != server.httpClients.end());
            server.httpClients.erase(it);
        }

        void sendFinalHttpResponse(std::experimental::string_view response) {
            if (!SendHttpResponses || response.empty()) {
                removeFromServer();
                return;
            }

            buffer.reset();

            boost::asio::async_write(*socket, boost::asio::buffer(response.cbegin(), response.length()),
                [this, reference = std::weak_ptr<HttpClient>(this->shared_from_this())] (const boost::system::error_code &e, std::size_t /*bytesSent*/) {
                    if (reference.expired()) {
                        return;
                    }

                    if (e && server.verboseLogging) {
                        std::cerr << "status HTTP header write error: " << e.message() << std::endl;
                    }

                    removeFromServer();
                });
        }

        void validateHttpHeader() {
            boost::asio::async_read_until(*socket, *buffer,
                UntilFunction(matchHttpHeader),
                [this, reference = std::weak_ptr<HttpClient>(this->shared_from_this()), bufferCapture = buffer] (const boost::system::error_code &e, size_t /*size*/) {
                    if (reference.expired()) {
                        return;
                    }

                    if (e) {
                        if (server.verboseLogging) {
                            std::cerr << "HTTP client error: " << e.message() << std::endl;
                        }

                        if (e == boost::system::errc::operation_canceled) {
                            sendFinalHttpResponse(
                                "HTTP/1.1 408 Request Timeout\r\n"
                                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                                "Connection: close\r\n"
                                "\r\n");
                        } else {
                            removeFromServer();
                        }

                        return;
                    }

                    if (!matchHttpHeader.success) {
                        if (server.verboseLogging) {
                            std::cerr << "HTTP client error: request header size is too large" << std::endl;
                        }

                        sendFinalHttpResponse(
                            "431 Request Header Fields Too Large\r\n"
                            "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                            "Content-Type: text/plain\r\n"
                            "Connection: close\r\n"
                            "\r\n"
                            "Total size of HTTP request header is too large");
                        return;
                    }

                    if (server.requestHandler) {
                        httpMethod = {matchHttpHeader.methodBegin, static_cast<size_t>(matchHttpHeader.methodEnd - matchHttpHeader.methodBegin)};
                        httpUri = {matchHttpHeader.uriBegin, static_cast<size_t>(matchHttpHeader.uriEnd - matchHttpHeader.uriBegin)};
                        protocolVersion = {matchHttpHeader.protocolVersionBegin, static_cast<size_t>(matchHttpHeader.protocolVersionEnd - matchHttpHeader.protocolVersionBegin)};
                        httpHeaderFields = {matchHttpHeader.headerFieldsBegin, static_cast<size_t>(matchHttpHeader.headerFieldsEnd - matchHttpHeader.headerFieldsBegin)};
                        server.requestHandler(std::make_shared<HttpRequest>(this->shared_from_this()));
                    } else {
                        removeFromServer();
                    }
                });
        }

        class MatchHttpHeader {
        public:
            explicit MatchHttpHeader(size_t maxHeaderSize) noexcept
                    : maxHeaderSize(maxHeaderSize),
                      currentMatcher(std::bind(&MatchHttpHeader::methodMatcher, this, std::placeholders::_1, std::placeholders::_2)) {
            }

            std::pair<UntilIterator, bool> operator()(UntilIterator begin, UntilIterator end) noexcept {
                return currentMatcher(begin, end);
            }

            std::pair<UntilIterator, bool> methodMatcher(UntilIterator begin, UntilIterator end) noexcept {
                if (methodBegin == nullptr) {
                    methodBegin = &*begin;
                }

                UntilIterator i = begin;

                while (i != end) {
                    if (++bytesRead == maxHeaderSize) {
                        return std::make_pair(i, true);
                    }

                    char c = *i;

                    // TODO: validate METHOD chars

                    if (c == ' ') {
                        methodEnd =  &*i;
                        currentMatcher = std::bind(&MatchHttpHeader::uriMatcher, this, std::placeholders::_1, std::placeholders::_2);
                        i++;
                        return uriMatcher(i, end);
                    }

                    i++;
                }

                return std::make_pair(i, false);
            }

            std::pair<UntilIterator, bool> uriMatcher(UntilIterator begin, UntilIterator end) noexcept {
                if (uriBegin == nullptr) {
                    uriBegin = &*begin;
                }

                UntilIterator i = begin;

                while (i != end) {
                    if (++bytesRead == maxHeaderSize) {
                        return std::make_pair(i, true);
                    }

                    char c = *i;

                    // TODO: validate URI chars

                    if (c == ' ') {
                        uriEnd =  &*i;
                        currentMatcher = std::bind(&MatchHttpHeader::protocolVersionMatcher, this, std::placeholders::_1, std::placeholders::_2);
                        i++;
                        bytesRead++;
                        return protocolVersionMatcher(i, end);
                    }

                    i++;
                }

                return std::make_pair(i, false);
            }

            std::pair<UntilIterator, bool> protocolVersionMatcher(UntilIterator begin, UntilIterator end) noexcept {
                if (protocolVersionBegin == nullptr) {
                    protocolVersionBegin = &*begin;
                }

                UntilIterator i = begin;

                while (i != end) {
                    if (++bytesRead == maxHeaderSize) {
                        return std::make_pair(i, true);
                    }

                    char c = *i;

                    // TODO: validate HTTP version chars

                    if ((previousChar == '\r') && (c == '\n')) {
                        protocolVersionEnd =  &*(i-1);
                        currentMatcher = std::bind(&MatchHttpHeader::headerFieldsMatcher, this, std::placeholders::_1, std::placeholders::_2);
                        previousChar ='\n';
                        i++;
                        bytesRead++;
                        return headerFieldsMatcher(i, end);
                    }

                    previousChar = c;
                    i++;
                }

                return std::make_pair(i, false);
            }

            std::pair<UntilIterator, bool> headerFieldsMatcher(UntilIterator begin, UntilIterator end) noexcept {
                if (headerFieldsBegin == nullptr) {
                    headerFieldsBegin = &*begin;
                }

                UntilIterator i = begin;

                while (i != end) {
                    char c = *i;

                    // TODO: split and validate header fields

                    if ((previousChar == '\r') && (c == '\n')) {
                        if (newLine) {
                            headerFieldsEnd =  &*(i-1);
                            success = true;
                            return std::make_pair(i, true);
                        } else {
                            newLine = true;
                        }
                    } else if ((newLine != true) || (previousChar != '\n') || (c != '\r')) {
                        newLine = false;
                    }

                    previousChar = c;
                    i++;

                    if (++bytesRead == maxHeaderSize) {
                        return std::make_pair(i, true);
                    }
                }

                return std::make_pair(i, false);
            }

            size_t maxHeaderSize;
            size_t bytesRead = 0;
            std::function<std::pair<UntilIterator, bool>(UntilIterator begin, UntilIterator end) noexcept> currentMatcher;

            const char* methodBegin = nullptr;
            const char* methodEnd = nullptr;

            const char* uriBegin = nullptr;
            const char* uriEnd = nullptr;

            const char* protocolVersionBegin = nullptr;
            const char* protocolVersionEnd = nullptr;

            const char* headerFieldsBegin = nullptr;
            const char* headerFieldsEnd = nullptr;

            char previousChar = 0;
            bool newLine = true;
            bool success = false;
        };

        SimpleHttpServer &server;

        std::shared_ptr<tcp::socket> socket;
        std::shared_ptr<boost::asio::basic_streambuf<Allocator>> buffer;
        boost::asio::system_timer timeoutTimer;

        MatchHttpHeader matchHttpHeader;
        std::experimental::string_view httpMethod;
        std::experimental::string_view httpUri;
        std::experimental::string_view protocolVersion;
        std::experimental::string_view httpHeaderFields;
    };

    tcp::acceptor acceptor;
    std::list<std::shared_ptr<HttpClient>> httpClients;

    size_t maxHttpHeaderSize = 4 * 1024;
    boost::asio::system_timer::duration httpConnectionTimeout = 1s;
    size_t maxHttpClients = 0;
    bool verboseLogging = true;

    RequestHandler requestHandler;
};

}
