#pragma once

#include <boost/asio.hpp>
#include <boost/asio/system_timer.hpp>

#include <iostream>
#include <list>

#include "version.h"

namespace UdpProxy {

using boost::asio::ip::tcp;
using namespace std::chrono_literals;

template <bool SendHttpResponses>
class SimpleHttpServer {
public:
    class HttpRequest {
    public:
        explicit HttpRequest(std::shared_ptr<typename SimpleHttpServer::HttpClient> httpClient) noexcept
            : httpClient(httpClient), buffer(httpClient->buffer), uri(httpClient->uri), headerFields(httpClient->headerFields) {
        }

        HttpRequest(const HttpRequest&) = default;
        HttpRequest(HttpRequest&&) = default;

        ~HttpRequest() noexcept {
            if (!httpClient.expired()) {
                httpClient.lock()->removeFromServer();
            }
        }

        std::experimental::string_view getUri() const noexcept { return uri; }
        std::experimental::string_view getHeaderFields() const noexcept { return headerFields; }
        tcp::socket& getSocket() const { return *httpClient.lock()->socket.get(); }
        bool isExpired() const noexcept { return httpClient.expired(); }

        void cancelTimeout() const {
            if (!httpClient.expired()) {
                httpClient.lock()->timeoutTimer.cancel();
            }
        }

    private:
        std::weak_ptr<typename SimpleHttpServer::HttpClient> httpClient;
        std::shared_ptr<boost::asio::streambuf> buffer;
        std::experimental::string_view uri;
        std::experimental::string_view headerFields;
    };

    typedef std::function<void(std::shared_ptr<HttpRequest>)> RequestHandler;

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

        acceptor.async_accept(*socket, [this, socket] (const boost::system::error_code &e) mutable {
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
                client->validateHttpMethod();
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
        HttpClient(std::shared_ptr<tcp::socket> &socket, SimpleHttpServer &server)
                : server(server), socket(socket), buffer(std::make_shared<boost::asio::streambuf>(server.maxHttpHeaderSize)),
                  timeoutTimer(socket->get_io_service()) {
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

            timeoutTimer.expires_from_now();
            timeoutTimer.async_wait([this, reference = std::weak_ptr<HttpClient>(this->shared_from_this())] (const boost::system::error_code &e) {
                if (reference.expired()) {
                    return;
                }

                if (e != boost::system::errc::operation_canceled) {
                    socket->cancel();
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

        void validateHttpMethod() {
            static constexpr std::experimental::string_view REQUEST_METHOD = "GET "sv;

            boost::asio::async_read_until(*socket, *buffer,
                UntilFunction(MatchStringOrSize(REQUEST_METHOD, REQUEST_METHOD.length())),
                [this, reference = std::weak_ptr<HttpClient>(this->shared_from_this()), bufferCapture = buffer] (const boost::system::error_code &e, size_t size) {
                    if (reference.expired()) {
                        return;
                    }

                    try {
                        if (e == boost::system::errc::operation_canceled) {
                            throw ServerError(e.message(),
                                "HTTP/1.1 408 Request Timeout\r\n"
                                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                                "Connection: close\r\n"
                                "\r\n");
                        } else if (e) {
                            throw ServerError(e.message(), "");
                        }

                        std::experimental::string_view method(boost::asio::buffer_cast<const char*>(buffer->data()), REQUEST_METHOD.length());
                        if (REQUEST_METHOD != method) {
                            throw ServerError("method not supported",
                                "HTTP/1.1 501 Not Implemented\r\n"
                                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                                "Connection: close\r\n"
                                "\r\n");
                        }

                        buffer->consume(size);
                        bytesRead += size;
                        readHttpRequestUri();
                    } catch (const ServerError &e) {
                        if (server.verboseLogging) {
                            std::cerr << "HTTP client error: " << e.what() << std::endl;
                        }
                        sendFinalHttpResponse(e.http_response());
                    }
                });
        }

        void readHttpRequestUri() {
            static constexpr std::experimental::string_view HTTP_VERSION_ENDING = " HTTP/1.1\r\n"sv;

            boost::asio::async_read_until(*socket, *buffer,
                UntilFunction(MatchStringOrSize("\r\n", server.maxHttpHeaderSize - bytesRead - "\r\n"sv.length())),
                [this, reference = std::weak_ptr<HttpClient>(this->shared_from_this()), bufferCapture = buffer] (const boost::system::error_code &e, size_t size) {
                    if (reference.expired()) {
                        return;
                    }

                    try {
                        if (e == boost::system::errc::operation_canceled) {
                            throw ServerError(e.message(),
                                "HTTP/1.1 408 Request Timeout\r\n"
                                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                                "Connection: close\r\n"
                                "\r\n");
                        } else if (e) {
                            throw ServerError(e.message(), "");
                        } else if (size <= HTTP_VERSION_ENDING.length()) {
                            throw ServerError("request not supported",
                                "HTTP/1.1 400 Bad Request\r\n"
                                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                                "Connection: close\r\n"
                                "\r\n");
                        }

                        std::experimental::string_view ending = {boost::asio::buffer_cast<const char*>(buffer->data()) + size - HTTP_VERSION_ENDING.length(), HTTP_VERSION_ENDING.length()};
                        if (HTTP_VERSION_ENDING != ending) {
                            throw ServerError("request not supported",
                                "HTTP/1.1 505 HTTP Version Not Supported\r\n"
                                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                                "Content-Type: text/plain\r\n"
                                "Connection: close\r\n"
                                "\r\n"
                                "Only HTTP/1.1 is supported");
                        }

                        uri = {boost::asio::buffer_cast<const char*>(buffer->data()), size - HTTP_VERSION_ENDING.length()};

                        buffer->consume(size - 2); // Do not consume CRLF
                        bytesRead += (size - 2);
                        readRestOfHttpHeader();
                    } catch (const ServerError &e) {
                        if (server.verboseLogging) {
                            std::cerr << "HTTP client error: " << e.what() << std::endl;
                        }
                        sendFinalHttpResponse(e.http_response());
                    }
                });
        }

        void readRestOfHttpHeader() {
            constexpr std::experimental::string_view HEADER_END = "\r\n\r\n"sv;

            boost::asio::async_read_until(*socket, *buffer,
                UntilFunction(MatchStringOrSize(HEADER_END, server.maxHttpHeaderSize - bytesRead)),
                [this, reference = std::weak_ptr<HttpClient>(this->shared_from_this()), bufferCapture = buffer] (const boost::system::error_code &e, size_t size) mutable {
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

                    if ((size < HEADER_END.length()) || (std::experimental::string_view(boost::asio::buffer_cast<const char*>(buffer->data()) + size - HEADER_END.length(), HEADER_END.length()) != HEADER_END)) {
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
                        headerFields = {boost::asio::buffer_cast<const char*>(buffer->data()) + 2, size - HEADER_END.length()};
                        server.requestHandler(std::make_shared<HttpRequest>(this->shared_from_this()));
                    } else {
                        removeFromServer();
                    }
                });
        }

        SimpleHttpServer &server;

        std::shared_ptr<tcp::socket> socket;
        std::shared_ptr<boost::asio::streambuf> buffer;
        size_t bytesRead = 0;
        boost::asio::system_timer timeoutTimer;

        std::experimental::string_view uri;
        std::experimental::string_view headerFields;
    };

    class MatchStringOrSize {
    public:
        explicit MatchStringOrSize(std::experimental::string_view string, size_t sizeLimit) noexcept : string(string), sizeLimit(sizeLimit) {}

        std::pair<UntilIterator, bool> operator()(UntilIterator begin, UntilIterator end) noexcept {
            UntilIterator i = begin;

            while (i != end) {
                char c = *i++;

                if (string[stringIndex] == c) {
                    if (++stringIndex == string.length()) {
                        return std::make_pair(i, true);
                    }
                } else {
                    stringIndex = 0;
                }

                if (++bytesRead == sizeLimit) {
                    return std::make_pair(i, true);
                }
            }

            return std::make_pair(i, false);
        }

    private:
        std::experimental::string_view string;
        size_t stringIndex = 0;
        size_t sizeLimit;
        size_t bytesRead = 0;
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
