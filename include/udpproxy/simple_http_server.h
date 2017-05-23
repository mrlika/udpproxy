#pragma once

#include "http_header_parser.h"

#include <boost/asio.hpp>
#include <boost/asio/system_timer.hpp>

#include <iostream>
#include <list>

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
        std::shared_ptr<std::vector<char, typename SimpleHttpServer::HttpClient::BufferAllocator>> buffer;
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
    void setRequestHandler(RequestHandler handler) noexcept { requestHandler = handler; }
    RequestHandler getRequestHandler() const noexcept { return requestHandler; }

private:
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
                : server(server), socket(socket), buffer(std::make_shared<typename decltype(buffer)::element_type>(server.maxHttpHeaderSize)),
                  timeoutTimer(socket->get_io_service()), httpHeaderParser{server.maxHttpHeaderSize} {
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

        void removeFromServer() noexcept {
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

        void validateHttpHeader(size_t position = 0, size_t bytesRead = 0) {
            auto bytesToRead = buffer->size() - bytesRead;
            assert(bytesToRead >= 0);
            if (bytesToRead == 0) {
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

            socket->async_read_some(boost::asio::buffer(buffer->data() + bytesRead, buffer->size() - bytesRead),
                [this, reference = std::weak_ptr<HttpClient>(this->shared_from_this()), buffer = buffer, position, bytesRead] (const boost::system::error_code &e, size_t size) {
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

                    const auto newBytesRead = bytesRead + size;

                    auto begin = buffer->cbegin() + position;
                    auto end = buffer->cbegin() + newBytesRead;

                    auto result = httpHeaderParser(begin, end);

                    if (!result.second) {
                        validateHttpHeader(position + result.first - begin, newBytesRead);
                        return;
                    }

                    if (!httpHeaderParser.isSucceeded()) {
                        if (server.verboseLogging) {
                            std::cerr << "HTTP client error: bad  HTTP request header" << std::endl;
                        }

                        sendFinalHttpResponse(
                            "400 Bad Request\r\n"
                            "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                            "Connection: close\r\n"
                            "\r\n");
                        return;
                    }

                    if (server.requestHandler) {
                        httpMethod = httpHeaderParser.getMethod();
                        httpUri = httpHeaderParser.getUri();
                        protocolVersion = httpHeaderParser.getProtocolVersion();
                        httpHeaderFields = httpHeaderParser.getHeaderFields();
                        server.requestHandler(std::make_shared<HttpRequest>(this->shared_from_this()));
                    } else {
                        removeFromServer();
                    }
                });
        }

        typedef typename std::allocator_traits<Allocator>::template rebind_alloc<std::vector<char>> BufferAllocator;

        SimpleHttpServer &server;

        std::shared_ptr<tcp::socket> socket;
        std::shared_ptr<std::vector<char, BufferAllocator>> buffer;
        boost::asio::system_timer timeoutTimer;

        HttpHeaderParser<typename decltype(buffer)::element_type::const_iterator> httpHeaderParser;
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
