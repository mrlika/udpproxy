#pragma once

#include "http_header_parser.h"

#include <boost/asio.hpp>
#include <boost/asio/system_timer.hpp>

#include <iostream>
#include <list>

namespace UdpProxy {

using boost::asio::ip::tcp;
using namespace std::chrono_literals;

enum class RequestError {
    ClientsLimitReached,
    HttpHeaderTooLarge,
    BadHttpRequest,
    RequestTimeout
};

struct SocketStreamFactory {
    typedef tcp::socket StreamType;

    static std::shared_ptr<StreamType> create(boost::asio::io_service &ioService) {
        return std::make_shared<StreamType>(ioService);
    }

    static void startAsync(std::shared_ptr<StreamType> &stream, std::function<void(const boost::system::error_code& error)> handler) {
        stream->get_io_service().post([handler] () { handler(boost::system::error_code()); });
    }
};

template <typename RequestHandler, typename StreamFactory, typename Allocator>
class SimpleHttpServer;

template <typename RequestHandler, typename StreamFactory = SocketStreamFactory, typename Allocator = std::allocator<char>>
class HttpRequest {
public:
    typedef typename std::remove_reference<StreamFactory>::type::StreamType StreamType;

    explicit HttpRequest(const std::shared_ptr<typename SimpleHttpServer<RequestHandler, StreamFactory, Allocator>::HttpClient> &httpClient) noexcept
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
    std::weak_ptr<StreamType> getStream() const {
        return httpClient.expired()
                ? std::weak_ptr<StreamType>()
                : std::weak_ptr<StreamType>(httpClient.lock()->stream);
    }

    void cancelTimeout() const {
        if (!httpClient.expired()) {
            httpClient.lock()->timeoutTimer.cancel();
        }
    }

private:
    std::weak_ptr<typename SimpleHttpServer<RequestHandler, StreamFactory, Allocator>::HttpClient> httpClient;
    std::shared_ptr<std::vector<char, typename SimpleHttpServer<RequestHandler, StreamFactory, Allocator>::HttpClient::BufferAllocator>> buffer;
    std::experimental::string_view method;
    std::experimental::string_view uri;
    std::experimental::string_view protocolVersion;
    std::experimental::string_view headerFields;
};

template <typename RequestHandler, typename StreamFactory = SocketStreamFactory, typename Allocator = std::allocator<char>>
class SimpleHttpServer {
public:
    typedef typename std::remove_reference<StreamFactory>::type::StreamType StreamType;

    SimpleHttpServer(boost::asio::io_service &ioService, const tcp::endpoint &endpoint, RequestHandler requestHandler = typename std::remove_reference<RequestHandler>::type(), StreamFactory streamFactory = typename std::remove_reference<StreamFactory>::type())
            : acceptor(ioService, endpoint), requestHandler(requestHandler), streamFactory(streamFactory) {}

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

private:
    template <typename RequestHandlerF, typename StreamFactoryF, typename AllocatorF>
    friend class HttpRequest;

    void startAccept() {
        auto stream = streamFactory.create(acceptor.get_io_service());

        acceptor.async_accept(stream->lowest_layer(), [this, stream = stream] (const boost::system::error_code &e) mutable {
            if (e) {
                if (e == boost::system::errc::operation_canceled) {
                    return;
                }

                std::cerr << "TCP accept error: " << e.message() << std::endl;
                return; // FIXME: is it good to stop accept loop?
            }

            auto client = std::make_shared<HttpClient>(stream, *this);
            httpClients.emplace_back(client);
            client->startCancelTimer();

            if ((maxHttpClients == 0) || (httpClients.size() <= maxHttpClients)) {
                streamFactory.startAsync(stream, [verboseLogging = this->verboseLogging, client = std::weak_ptr<HttpClient>(client->shared_from_this())] (const boost::system::error_code &e) {
                    if (client.expired()) {
                        return;
                    }

                    if (e) {
                        if (verboseLogging) {
                            std::cerr << "HTTP client start error: " << e.message() << std::endl;
                        }

                        client.lock()->removeFromServer();
                        return;
                    }

                    client.lock()->validateHttpHeader();
                });
            } else {
                if (verboseLogging) {
                    std::cerr << "Maximum of HTTP clients reached. Connection refused: " << stream->lowest_layer().remote_endpoint() << std::endl;
                }

                // FIXME: initialize connection (i.e. SSL) using streamFactory.startAsync here?
                requestHandler.handleRequestError(std::make_shared<HttpRequest<RequestHandler, StreamFactory, Allocator>>(client), RequestError::ClientsLimitReached);
            }

            startAccept();
        });
    }

    struct HttpClient : public std::enable_shared_from_this<HttpClient> {
        HttpClient(const std::shared_ptr<StreamType> &stream, SimpleHttpServer &server)
                : server(server), stream(stream), remoteEndpoint(stream->lowest_layer().remote_endpoint()), buffer(std::make_shared<typename decltype(buffer)::element_type>(server.maxHttpHeaderSize)),
                  timeoutTimer(stream->get_io_service()), httpHeaderParser{server.maxHttpHeaderSize} {
            if (server.verboseLogging) {
                std::cerr << "new connection " << remoteEndpoint << std::endl;
            }
        }

        ~HttpClient() noexcept {
            if (server.verboseLogging) {
                std::cerr << "remove connection " << remoteEndpoint << std::endl;
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
                    stream->lowest_layer().cancel();
                    timeoutTimer.expires_from_now(server.httpConnectionTimeout);
                    timeoutTimer.async_wait([this, reference = std::weak_ptr<HttpClient>(this->shared_from_this())] (const boost::system::error_code &e) {
                        if (!reference.expired() && (e != boost::system::errc::operation_canceled)) {
                            stream->lowest_layer().cancel();
                        }
                    });
                    server.requestHandler.handleRequestError(std::make_shared<HttpRequest<RequestHandler, StreamFactory, Allocator>>(this->shared_from_this()), RequestError::RequestTimeout);
                }
            });
        }

        void removeFromServer() noexcept {
            auto it = find_if(server.httpClients.begin(), server.httpClients.end(), [this] (const std::shared_ptr<HttpClient>& client) { return client.get() == this; });
            assert(it != server.httpClients.end());
            server.httpClients.erase(it);
        }

        void validateHttpHeader(size_t position = 0, size_t bytesRead = 0) {
            auto bytesToRead = buffer->size() - bytesRead;
            assert(bytesToRead >= 0);
            if (bytesToRead == 0) {
                if (server.verboseLogging) {
                    std::cerr << "HTTP client error: request header size is too large" << std::endl;
                }

                server.requestHandler.handleRequestError(std::make_shared<HttpRequest<RequestHandler, StreamFactory, Allocator>>(this->shared_from_this()), RequestError::HttpHeaderTooLarge);
                return;
            }

            stream->async_read_some(boost::asio::buffer(buffer->data() + bytesRead, buffer->size() - bytesRead),
                [this, reference = std::weak_ptr<HttpClient>(this->shared_from_this()), buffer = buffer, position, bytesRead] (const boost::system::error_code &e, size_t size) {
                    if (reference.expired()) {
                        return;
                    }

                    if (e) {
                        if (server.verboseLogging) {
                            std::cerr << "HTTP client error: " << e.message() << std::endl;
                        }

                        removeFromServer();
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

                        server.requestHandler.handleRequestError(std::make_shared<HttpRequest<RequestHandler, StreamFactory, Allocator>>(this->shared_from_this()), RequestError::BadHttpRequest);
                        return;
                    }

                    httpMethod = httpHeaderParser.getMethod();
                    httpUri = httpHeaderParser.getUri();
                    protocolVersion = httpHeaderParser.getProtocolVersion();
                    httpHeaderFields = httpHeaderParser.getHeaderFields();
                    server.requestHandler.handleRequest(std::make_shared<HttpRequest<RequestHandler, StreamFactory, Allocator>>(this->shared_from_this()));
                });
        }

        typedef typename std::allocator_traits<Allocator>::template rebind_alloc<std::vector<char>> BufferAllocator;

        SimpleHttpServer &server;

        std::shared_ptr<StreamType> stream;
        tcp::endpoint remoteEndpoint;
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
    StreamFactory streamFactory;
};

}
