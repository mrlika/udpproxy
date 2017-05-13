#pragma once

#include <boost/asio.hpp>
#include <boost/asio/system_timer.hpp>
#include <boost/lexical_cast.hpp>

#include <cassert>
#include <iostream>
#include <algorithm>
#include <regex>
#include <unordered_map>
#include <list>
#include <experimental/string_view>
#include <experimental/array>

namespace UdpProxy {

using boost::asio::ip::tcp;
using boost::asio::ip::udp;
using namespace std::chrono_literals;
using namespace std::experimental::string_view_literals;

constexpr std::tuple<unsigned, unsigned, unsigned> VERSION {0, 1, 0};
constexpr std::experimental::string_view SERVER_NAME = "udpproxy 0.1.0"sv;
#define UDPPROXY_SERVER_NAME_DEFINE "udpproxy 0.1.0"

enum class OutputQueueOverflowAlgorithm {
    ClearQueue,
    DropData
};

template <typename Allocator>
class UdpServer {
public:
    explicit UdpServer(boost::asio::io_service &ioService) : ioService(ioService), clientsReadTimer(ioService) {
        static constexpr size_t CLIENT_READ_BUFFER_SIZE = 1024;
        clientsReadBuffer = std::make_shared<std::vector<uint8_t, InputBuffersAllocator>>(CLIENT_READ_BUFFER_SIZE);
    }

    void setVerboseLogging(bool value) noexcept { verboseLogging = value; }
    bool getVerboseLogging() const noexcept { return verboseLogging; }
    void setMaxUdpDataSize(size_t value) noexcept { maxUdpDataSize = value; }
    size_t getMaxUdpDataSize() const noexcept { return maxUdpDataSize; }
    void setMaxOutputQueueLength(size_t value) noexcept { maxOutputQueueLength = value; }
    size_t getMaxOutputQueueLength() const noexcept { return maxOutputQueueLength; }
    void setOutputQueueOverflowAlgorithm(OutputQueueOverflowAlgorithm value) noexcept { overflowAlgorithm = value; }
    OutputQueueOverflowAlgorithm getOutputQueueOverflowAlgorithm() const noexcept { return overflowAlgorithm; };
    void setRenewMulticastSubscriptionInterval(boost::asio::system_timer::duration value) noexcept { renewMulticastSubscriptionInterval = value; }
    boost::asio::system_timer::duration getRenewMulticastSubscriptionInterval() const noexcept { return renewMulticastSubscriptionInterval; }
    void setMulticastInterfaceAddress(boost::asio::ip::address value) noexcept { multicastInterfaceAddress = value; }
    boost::asio::ip::address getMulticastInterfaceAddress() const noexcept { return multicastInterfaceAddress; }

    void runAsync() {
        readClients();
    }

    void addUdpToHttpClient(std::shared_ptr<tcp::socket> &clientSocket, const boost::asio::ip::udp::endpoint &udpEndpoint) {
        uint64_t inputId = getEndpointId(udpEndpoint);

        auto udpInputIterator = udpInputs.find(inputId);
        UdpInput *udpInput;

        if (udpInputIterator == udpInputs.end()) {
            std::unique_ptr<UdpInput> udpInputUnique;

            udpInputUnique = std::make_unique<UdpInput>(*this, inputId, udpEndpoint);

            udpInput = udpInputUnique.get();
            udpInputs.emplace(inputId, std::move(udpInputUnique));
        } else {
            udpInput = udpInputIterator->second.get();
        }

        udpInput->addClient(clientSocket);
    }

    void removeUdpToHttpClient(const boost::asio::ip::tcp::endpoint &clientEndpoint, const boost::asio::ip::udp::endpoint &udpEndpoint) {
        auto udpInputIterator = udpInputs.find(getEndpointId(udpEndpoint));
        if (udpInputIterator == udpInputs.end()) {
            return;
        }

        auto& clients = udpInputIterator->second->clients;

        auto it = std::find_if(clients.begin(), clients.end(), [&clientEndpoint] (const std::shared_ptr<typename UdpInput::Client> &client) { return client->remoteEndpoint == clientEndpoint; });
        if (it == clients.end()) {
            return;
        }

        clients.erase(it);

        if (clients.empty()) {
            udpInputs.erase(udpInputIterator);
        }
    }

private:
    typedef typename std::allocator_traits<Allocator>::template rebind_alloc<std::vector<uint8_t>> InputBuffersAllocator;

    struct UdpInput : public std::enable_shared_from_this<UdpServer::UdpInput> {
        UdpInput(UdpServer &udpServer, uint64_t id, const boost::asio::ip::udp::endpoint &udpEndpoint)
                : udpServer(udpServer), id(id), udpSocket(udpServer.ioService),
                  udpEndpoint(udpEndpoint), renewMulticastSubscriptionTimer(udpServer.ioService) {
            udpSocket.open(udpEndpoint.protocol());
            udpSocket.set_option(boost::asio::ip::udp::socket::reuse_address(true)); // FIXME: is it good?
            udpSocket.bind(udpEndpoint);

            if (udpEndpoint.address().is_multicast()) {
                udpSocket.set_option(boost::asio::ip::multicast::join_group(udpEndpoint.address().to_v4(), udpServer.multicastInterfaceAddress.to_v4()));
            }

            if (udpServer.verboseLogging) {
                std::cerr << "new UDP input: udp://" << udpEndpoint << std::endl;
            }

            if (udpServer.newUdpInputCallback) {
                udpServer.newUdpInputCallback(udpEndpoint);
            }
        }

        ~UdpInput() {
            if (udpServer.verboseLogging) {
                std::cerr << "remove UDP input: " << udpEndpoint << std::endl;
            }

            if (udpServer.removeUdpInputCallback) {
                udpServer.removeUdpInputCallback(udpEndpoint);
            }
        }

        void startRenewMulticastSubscription() {
            renewMulticastSubscriptionTimer.expires_from_now(udpServer.renewMulticastSubscriptionInterval);
            renewMulticastSubscriptionTimer.async_wait([this, reference = std::weak_ptr<UdpInput>(this->shared_from_this())] (const boost::system::error_code &e) {
                if (reference.expired()) {
                    return;
                } else if (e) {
                    return;
                }

                try {
                    if (udpServer.verboseLogging) {
                        std::cerr << "renew multicast subscription for " << udpEndpoint << std::endl;
                    }
                    udpSocket.set_option(boost::asio::ip::multicast::leave_group(udpEndpoint.address()));
                    udpSocket.set_option(boost::asio::ip::multicast::join_group(udpEndpoint.address()));
                } catch (const boost::system::system_error &e) {
                    std::cerr << "error: failed to renew multicast subscription for " << udpEndpoint << ": " << e.what() << std::endl;
                    udpServer.udpInputs.erase(id);
                    return;
                }

                startRenewMulticastSubscription();
            });
        }

        void start() {
            if (!isStarted) {
                isStarted = true;

                if (udpEndpoint.address().is_multicast() && (udpServer.renewMulticastSubscriptionInterval != 0s)) {
                    startRenewMulticastSubscription();
                }

                if (udpServer.startUdpInputCallback) {
                    udpServer.startUdpInputCallback(udpEndpoint);
                }

                receiveUdp();
            }
        }

        void receiveUdp() {
            inputBuffer = std::make_shared<std::vector<uint8_t, InputBuffersAllocator>>(udpServer.maxUdpDataSize);

            udpSocket.async_receive_from(boost::asio::buffer(inputBuffer->data(), inputBuffer->size()), senderEndpoint,
                [this, reference = std::weak_ptr<UdpInput>(this->shared_from_this()), buffer = inputBuffer] (const boost::system::error_code &e, std::size_t bytesRead) {
                    if (reference.expired()) {
                        return;
                    }

                    if (e) {
                        std::cerr << "UDP socket receive error for " << udpEndpoint << ": " << e.message() << std::endl;
                        udpServer.udpInputs.erase(id);
                        return;
                    }

                    if (udpServer.readUdpInputCallback) {
                        udpServer.readUdpInputCallback(udpEndpoint, bytesRead);
                    }

                    inputBuffer->resize(bytesRead);

                    for (auto& client : clients) {
                        size_t length = client->outputBuffers.size();

                        if (length == 0) {
                            client->outputBuffers.emplace_back(inputBuffer);
                            client->writeData(inputBuffer);
                        } else if ((udpServer.maxOutputQueueLength == 0) || (length < udpServer.maxOutputQueueLength)) {
                            client->outputBuffers.emplace_back(inputBuffer);
                        } else {
                            switch (udpServer.overflowAlgorithm) {
                            case OutputQueueOverflowAlgorithm::ClearQueue:
                                if (udpServer.verboseLogging) {
                                    std::cerr << "error: output queue overflow - clearing queue for " << client->remoteEndpoint << " (udp://" << udpEndpoint << ")" << std::endl;
                                }
                                client->outputBuffers.resize(1);
                                break;

                            case OutputQueueOverflowAlgorithm::DropData:
                                if (udpServer.verboseLogging) {
                                    std::cerr << "error: output queue overflow - dropping data for " << client->remoteEndpoint << " (udp://" << udpEndpoint << ")" << std::endl;
                                }
                                break;
                            }
                        }
                    }

                    receiveUdp();
                });
        }

        void addClient(std::shared_ptr<tcp::socket>& socket) {
            auto client = std::make_shared<typename UdpInput::Client>(socket, udpServer, id, udpEndpoint);
            clients.emplace_back(client);
            client->writeHttpHeader();
        }

        struct Client : public std::enable_shared_from_this<UdpInput::Client> {
            Client(std::shared_ptr<tcp::socket> &socket, UdpServer &udpServer, uint64_t inputId, const boost::asio::ip::udp::endpoint& udpEndpoint) noexcept
                    : socket(socket), udpServer(udpServer), inputId(inputId), remoteEndpoint(socket->remote_endpoint()), udpEndpoint(udpEndpoint) {
                if (udpServer.verboseLogging) {
                    std::cerr << "new HTTP client " << remoteEndpoint << " for " << udpEndpoint <<  std::endl;
                }

                if (udpServer.newClientCallback) {
                    udpServer.newClientCallback(remoteEndpoint, udpEndpoint);
                }
            }

            ~Client() noexcept {
                if (udpServer.verboseLogging) {
                    std::cerr << "remove HTTP client " << remoteEndpoint << " for " << udpEndpoint << std::endl;
                }

                if (udpServer.removeClientCallback) {
                    udpServer.removeClientCallback(remoteEndpoint, udpEndpoint);
                }
            }

            void writeData(std::shared_ptr<std::vector<uint8_t, InputBuffersAllocator>> &buffer) {
                boost::asio::async_write(*socket, boost::asio::buffer(buffer->data(), buffer->size()),
                    [this, reference = std::weak_ptr<Client>(this->shared_from_this()), buffer = buffer] (const boost::system::error_code &e, std::size_t bytesSent) {
                        if (reference.expired()) {
                            return;
                        }

                        if (e) {
                            if (udpServer.verboseLogging) {
                                std::cerr << "HTTP write error for " << remoteEndpoint << ": " << e.message() << std::endl;
                            }
                            udpServer.removeUdpToHttpClient(inputId, socket);
                            return;
                        }

                        if (udpServer.writeClientCallback) {
                            udpServer.writeClientCallback(remoteEndpoint, bytesSent);
                        }

                        assert(buffer == outputBuffers.front());
                        (void)bytesSent; // Avoid unused parameter warning when asserts disabled
                        assert(bytesSent == outputBuffers.front()->size());

                        outputBuffers.pop_front();
                        if (!outputBuffers.empty()) {
                            writeData(outputBuffers.front());
                        }
                    });
            }

            void doReadCheck() {
                if (!readSomeDone) {
                    return;
                }

                readSomeDone = false;
                socket->async_read_some(boost::asio::buffer(udpServer.clientsReadBuffer->data(), udpServer.clientsReadBuffer->size()),
                    [this, reference = std::weak_ptr<Client>(this->shared_from_this()), buffer = udpServer.clientsReadBuffer] (const boost::system::error_code &e, std::size_t /*bytesRead*/) mutable {
                        if (reference.expired()) {
                            return;
                        }

                        if (!e) {
                            return;
                        } else if (udpServer.verboseLogging) {
                            std::cerr << "error reading client " << remoteEndpoint << ": " << e.message() << std::endl;
                        }

                        udpServer.removeUdpToHttpClient(inputId, socket);
                     });
            }

            void writeHttpHeader() {
                static constexpr std::experimental::string_view HTTP_RESPONSE_HEADER =
                    "HTTP/1.1 200 OK\r\n"
                    "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                    "Content-Type: application/octet-stream\r\n"
                    "Connection: close\r\n"
                    "\r\n"sv;

                boost::asio::async_write(*socket, boost::asio::buffer(HTTP_RESPONSE_HEADER.cbegin(), HTTP_RESPONSE_HEADER.length()),
                    [this, reference = std::weak_ptr<Client>(this->shared_from_this())] (const boost::system::error_code &e, std::size_t /*bytesSent*/) {
                        if (reference.expired()) {
                            return;
                        }

                        if (e) {
                            if (udpServer.verboseLogging) {
                                std::cerr << "HTTP header write error for " << remoteEndpoint << ": " << e.message() << std::endl;
                            }

                            udpServer.removeUdpToHttpClient(inputId, socket);
                            return;
                        }

                        auto udpInputIterator = udpServer.udpInputs.find(inputId);
                        assert(udpInputIterator != udpServer.udpInputs.end());
                        udpInputIterator->second->start();
                    });
            }

            std::shared_ptr<tcp::socket> socket;
            UdpServer &udpServer;
            uint64_t inputId;
            boost::asio::ip::tcp::endpoint remoteEndpoint;
            boost::asio::ip::udp::endpoint udpEndpoint;
            std::list<std::shared_ptr<std::vector<uint8_t, InputBuffersAllocator>>> outputBuffers;
            bool readSomeDone = true;
        };

        UdpServer &udpServer;
        uint64_t id;
        std::list<std::shared_ptr<UdpInput::Client>> clients;
        udp::socket udpSocket;
        udp::endpoint senderEndpoint;
        udp::endpoint udpEndpoint;
        std::shared_ptr<std::vector<uint8_t, InputBuffersAllocator>> inputBuffer;
        bool isStarted = false;
        boost::asio::system_timer renewMulticastSubscriptionTimer;
    };

    static uint64_t getEndpointId(const boost::asio::ip::udp::endpoint &udpEndpoint) noexcept {
        return (static_cast<uint64_t>(udpEndpoint.address().to_v4().to_ulong()) << 16) | udpEndpoint.port();
    }

    void removeUdpToHttpClient(uint64_t inputId, const std::shared_ptr<tcp::socket> &clientSocket) {
        auto udpInputIterator = udpInputs.find(inputId);
        assert(udpInputIterator != udpInputs.end());

        auto& clients = udpInputIterator->second->clients;

        auto it = std::find_if(clients.begin(), clients.end(), [&clientSocket] (const std::shared_ptr<typename UdpInput::Client> &client) { return client->socket == clientSocket; });
        assert(it != clients.end());
        clients.erase(it);

        if (clients.empty()) {
            udpInputs.erase(udpInputIterator);
        }
    }

    void readClients() {
        // Slowly read clients' sockets to detect disconnected ones

        static constexpr boost::asio::system_timer::duration CLIENT_READ_PERIOD = 5s;

        clientsReadTimer.expires_from_now(CLIENT_READ_PERIOD);
        clientsReadTimer.async_wait([this] (const boost::system::error_code &e) {
            if (e == boost::system::errc::operation_canceled) {
                return;
            }

            for (auto& udpInput : udpInputs) {
                for (auto& client : udpInput.second->clients) {
                    client->doReadCheck();
                }
            }

            readClients();
        });
    }

    std::unordered_map<uint64_t, std::shared_ptr<UdpInput>> udpInputs;
    boost::asio::io_service &ioService;
    std::shared_ptr<std::vector<uint8_t, InputBuffersAllocator>> clientsReadBuffer;
    boost::asio::system_timer clientsReadTimer;

    bool verboseLogging = true;
    size_t maxUdpDataSize = 4 * 1024;
    size_t maxOutputQueueLength = 1024;
    OutputQueueOverflowAlgorithm overflowAlgorithm = OutputQueueOverflowAlgorithm::ClearQueue;
    boost::asio::system_timer::duration renewMulticastSubscriptionInterval = 0s;
    boost::asio::ip::address multicastInterfaceAddress;

    std::function<void(const boost::asio::ip::udp::endpoint &udpEndpoint)> newUdpInputCallback;
    std::function<void(const boost::asio::ip::udp::endpoint &udpEndpoint)> removeUdpInputCallback;
    std::function<void(const boost::asio::ip::udp::endpoint &udpEndpoint)> startUdpInputCallback;
    std::function<void(const boost::asio::ip::tcp::endpoint &clientEndpoint, const boost::asio::ip::udp::endpoint &udpEndpoint)> newClientCallback;
    std::function<void(const boost::asio::ip::tcp::endpoint &clientEndpoint, const boost::asio::ip::udp::endpoint &udpEndpoint)> removeClientCallback;
    std::function<void(const boost::asio::ip::udp::endpoint &udpEndpoint, size_t bytesRead)> readUdpInputCallback;
    std::function<void(const boost::asio::ip::tcp::endpoint &clientEndpoint, size_t bytesWritten)> writeClientCallback;
};

template <bool SendHttpResponses>
class SimpleHttpServer {
private:
    class HttpHeaderReader;

public:
    class HttpRequest {
    public:
        ~HttpRequest() {
            if (!socket.expired()) {
                auto it = find_if(server.httpHeaderReaders.begin(), server.httpHeaderReaders.end(), [socketPointer = std::shared_ptr<tcp::socket>(socket).get()] (const std::shared_ptr<HttpHeaderReader>& reader) { return reader->socket.get() == socketPointer; });
                assert(it != server.httpHeaderReaders.end());
                server.httpHeaderReaders.erase(it);
            }
        }

        HttpRequest(HttpRequest&&) = default;

        std::experimental::string_view getUri() { return uri; }
        std::experimental::string_view getHeaderFields() { return headerFields; }
        std::weak_ptr<tcp::socket> getSocket() { return socket; }

        void cancelRequestTimeoutTimer() {
            if (!socket.expired()) {
                timeoutTimer.cancel();
            }
        }

    private:
        friend class HttpHeaderReader;

        HttpRequest(std::weak_ptr<tcp::socket> socket, std::shared_ptr<boost::asio::streambuf> headerBuffer,
                boost::asio::system_timer &timeoutTimer, std::experimental::string_view uri,
                std::experimental::string_view headerFields, SimpleHttpServer &server) noexcept
                : socket(socket), headerBuffer(headerBuffer), timeoutTimer(timeoutTimer), uri(uri), headerFields(headerFields), server(server) {
        }

        std::weak_ptr<tcp::socket> socket;
        std::shared_ptr<boost::asio::streambuf> headerBuffer;
        boost::asio::system_timer &timeoutTimer;
        std::experimental::string_view uri;
        std::experimental::string_view headerFields;
        SimpleHttpServer &server;
    };

    SimpleHttpServer(boost::asio::io_service &ioService, const tcp::endpoint &endpoint)
            : acceptor(ioService, endpoint) {}

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
    void setRequestHandler(std::function<void(HttpRequest)> handler) { requestHandler = handler; }
    std::function<void(HttpRequest)> getRequestHandler() { return requestHandler; }

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

            auto reader = std::make_shared<HttpHeaderReader>(socket, *this);
            httpHeaderReaders.emplace_back(reader);
            reader->startCancelTimer();

            if ((maxHttpClients == 0) || (httpHeaderReaders.size() <= maxHttpClients)) {
                reader->validateHttpMethod();
            } else {
                if (verboseLogging) {
                    std::cerr << "Maximum of HTTP clients reached. Connection refused: " << socket->remote_endpoint() << std::endl;
                }

                reader->sendFinalHttpResponse(
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

    struct HttpHeaderReader : public std::enable_shared_from_this<HttpHeaderReader> {
        HttpHeaderReader(std::shared_ptr<tcp::socket> &socket, SimpleHttpServer &server)
                : socket(socket), buffer(std::make_shared<boost::asio::streambuf>(server.maxHttpHeaderSize)),
                  timeoutTimer(socket->get_io_service()), server(server) {
            if (server.verboseLogging) {
                std::cerr << "new connection " << socket->remote_endpoint() << std::endl;
            }
        }

        ~HttpHeaderReader() noexcept {
            if (server.verboseLogging) {
                std::cerr << "remove connection " << socket->remote_endpoint() << std::endl;
            }
        }

        void startCancelTimer() {
            if (server.httpConnectionTimeout == 0s) {
                return;
            }

            timeoutTimer.expires_from_now(server.httpConnectionTimeout);
            timeoutTimer.async_wait([this, reference = std::weak_ptr<HttpHeaderReader>(this->shared_from_this())] (const boost::system::error_code &e) {
                if (reference.expired()) {
                    return;
                }

                if (e != boost::system::errc::operation_canceled) {
                    socket->cancel();
                }
            });
        }

        void removeFromServer() {
            auto it = find_if(server.httpHeaderReaders.begin(), server.httpHeaderReaders.end(), [this] (const std::shared_ptr<HttpHeaderReader>& reader) { return reader.get() == this; });
            assert(it != server.httpHeaderReaders.end());
            server.httpHeaderReaders.erase(it);
        }

        void sendFinalHttpResponse(std::experimental::string_view response) {
            if (!SendHttpResponses || response.empty()) {
                removeFromServer();
                return;
            }

            buffer.reset();

            boost::asio::async_write(*socket, boost::asio::buffer(response.cbegin(), response.length()),
                [this, reference = std::weak_ptr<HttpHeaderReader>(this->shared_from_this())] (const boost::system::error_code &e, std::size_t /*bytesSent*/) {
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
                [this, reference = std::weak_ptr<HttpHeaderReader>(this->shared_from_this()), bufferCapture = buffer] (const boost::system::error_code &e, size_t size) {
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
                [this, reference = std::weak_ptr<HttpHeaderReader>(this->shared_from_this()), bufferCapture = buffer] (const boost::system::error_code &e, size_t size) {
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
                [this, reference = std::weak_ptr<HttpHeaderReader>(this->shared_from_this()), bufferCapture = buffer] (const boost::system::error_code &e, size_t size) {
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
                        std::experimental::string_view httpHeaderFields = {boost::asio::buffer_cast<const char*>(buffer->data()) + 2, size - HEADER_END.length()};

                        HttpRequest httpRequest(socket, buffer, timeoutTimer, uri, httpHeaderFields, server);

                        server.requestHandler(std::move(httpRequest));
                    } else {
                        removeFromServer();
                    }
                });
        }

        std::shared_ptr<tcp::socket> socket;
        std::shared_ptr<boost::asio::streambuf> buffer;
        size_t bytesRead = 0;
        boost::asio::system_timer timeoutTimer;
        SimpleHttpServer &server;

        std::experimental::string_view uri;
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
    std::list<std::shared_ptr<HttpHeaderReader>> httpHeaderReaders;

    size_t maxHttpHeaderSize = 4 * 1024;
    boost::asio::system_timer::duration httpConnectionTimeout = 1s;
    size_t maxHttpClients = 0;
    bool verboseLogging = true;

    std::function<void(HttpRequest)> requestHandler;
};

template <typename Allocator, bool SendHttpResponses>
class BasicServer {
public:
    BasicServer(boost::asio::io_service &ioService, const tcp::endpoint &endpoint)
            : udpServer(ioService), httpServer(ioService, endpoint) {
        httpServer.setRequestHandler(std::bind(&BasicServer::handleRequest, this, std::placeholders::_1));
    }

    void runAsync() {
        udpServer.runAsync();
        httpServer.runAsync();
    }

    void setMaxHttpHeaderSize(size_t value) { httpServer.setMaxHttpHeaderSize(value); }
    size_t getMaxHttpHeaderSize() const noexcept { return httpServer.getMaxHttpHeaderSize(); }
    void setHttpConnectionTimeout(boost::asio::system_timer::duration value) { httpServer.setHttpConnectionTimeout(value); }
    boost::asio::system_timer::duration getHttpConnectionTimeout() const noexcept { return httpServer.getHttpConnectionTimeout(); }
    void setMaxUdpDataSize(size_t value) { udpServer.setMaxUdpDataSize(value); }
    size_t getMaxUdpDataSize() const noexcept { return udpServer.getMaxUdpDataSize(); }
    void setMaxOutputQueueLength(size_t value) { udpServer.setMaxOutputQueueLength(value); }
    size_t getMaxOutputQueueLength() const noexcept { return udpServer.getMaxOutputQueueLength(); }
    void setMaxHttpClients(size_t value) { httpServer.setMaxHttpClients(value); }
    size_t getMaxHttpClients() const noexcept { return httpServer.getMaxHttpClients(); }
    void setOutputQueueOverflowAlgorithm(OutputQueueOverflowAlgorithm value) { udpServer.setOutputQueueOverflowAlgorithm(value); }
    OutputQueueOverflowAlgorithm getOutputQueueOverflowAlgorithm() const noexcept { return udpServer.getOutputQueueOverflowAlgorithm(); };
    void setVerboseLogging(bool value) {
        verboseLogging = value;
        udpServer.setVerboseLogging(value);
        httpServer.setVerboseLogging(value);
    }
    bool getVerboseLogging() const noexcept { return verboseLogging; }
    void setEnableStatus(bool value) { enableStatus = value; }
    bool getEnableStatus() const noexcept { return enableStatus; }
    void setRenewMulticastSubscriptionInterval(boost::asio::system_timer::duration value) { udpServer.setRenewMulticastSubscriptionInterval(value); }
    boost::asio::system_timer::duration getRenewMulticastSubscriptionInterval() const noexcept { return udpServer.getRenewMulticastSubscriptionInterval(); }
    void setMulticastInterfaceAddress(boost::asio::ip::address value) { udpServer.setMulticastInterfaceAddress(value); }
    boost::asio::ip::address getMulticastInterfaceAddress() const noexcept { return udpServer.getMulticastInterfaceAddress(); }

private:
    void addUdpClient() {
        /*try {
            udpServer.addUdpToHttpClient(socket, udpEndpoint);
        } catch (const boost::system::system_error &e) {
            std::cerr << "UDP socket error for " << udpEndpoint << ": " << e.what() << std::endl;
            sendFinalHttpResponse(
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n"
                "\r\n"
                "UDP socket error");
            return;
        }*/
    }

    void parseUdpUri() {
        /*static constexpr size_t MIN_UDP_REQUEST_LINE_SIZE = "/udp/d.d.d.d:d"sv.length() + HTTP_VERSION_ENDING.length();
        static constexpr std::experimental::string_view STATUS_URI = "/status"sv;

        // TODO: replace regex with parsing algorithm for better performance and to avoid memory allocations
        static const std::regex uriRegex("/udp/(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}):(\\d{1,5})(?:\\?.*)?", std::regex_constants::optimize);
        std::cmatch match;
        std::regex_match(uri.begin(), uri.end(), match, uriRegex);

        if (match.empty()) {
            throw ServerError("wrong URI: " + std::string(uri),
                "HTTP/1.1 404 Not Found\r\n"
                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n"
                "\r\n"
                "404 Not Found");
        }

        boost::asio::ip::address address;
        unsigned long port;

        try {
            // TODO: use std::from_chars and match[2].first/second to avoid std::string creation and memory copying
            port = std::stoul(match[2]);
            // TODO: use string_view created from match[2].first/second to avoid std::string creation and memory copying
            // possible only when from_string supports string_view
            address = boost::asio::ip::address::from_string(match[1]);
        } catch (...) {
            throw ServerError("wrong URI: " + std::string(uri),
                "HTTP/1.1 404 Not Found\r\n"
                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n"
                "\r\n"
                "404 Not Found");
        }

        if ((port == 0) || (port > std::numeric_limits<uint16_t>::max())) {
            throw ServerError("wrong URI: " + std::string(uri),
                "HTTP/1.1 404 Not Found\r\n"
                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n"
                "\r\n"
                "404 Not Found");
        }

        udpEndpoint = {address, static_cast<unsigned short>(port)};*/
    }

    void writeJsonStatus() {
        /*if (verboseLogging) {
            std::cerr << "status HTTP client: " << socket->remote_endpoint() << std::endl;
        }

        // TODO: optimize JSON output
        std::ostringstream out;

        out << R"({"inputs":[)";
        bool first = true;
        for (const auto& udpInputItem : server.udpServer.udpInputs) {
            if (first) {
                first = false;
                out << '{';
            } else {
                out << ",{";
            }

            const auto& udpInput = *(udpInputItem.second);
            out << R"("endpoint":")" << udpInput.udpEndpoint << R"(",)";
            out << R"("clients_count":)" << udpInput.clients.size() << ',';
            out << R"("bitrate":)" << udpInput.inBitrateKbit << '}';
        }
        out << R"(],"clients":[)";
        first = true;
        for (const auto& udpInput : server.udpServer.udpInputs) {
            std::string udpEndpointJson = R"("udp_endpoint":")" + boost::lexical_cast<std::string>(udpInput.second->udpEndpoint) + R"(",)";
            for (const auto& client : udpInput.second->clients) {
                if (first) {
                    first = false;
                    out << '{';
                } else {
                    out << ",{";
                }

                out << R"("remote_endpoint":")" << client->remoteEndpoint << R"(",)";
                out << udpEndpointJson;
                out << R"("output_queue_length":)" << client->outputBuffers.size() << ',';
                out << R"("bitrate":)" << client->outBitrateKbit << '}';
            }
        }
        out << "]}";

        // TODO: optimize to use direct buffer access without copying
        auto response = std::make_shared<std::string>(out.str());

        static constexpr std::experimental::string_view HTTP_RESPONSE_HEADER =
            "HTTP/1.1 200 OK\r\n"
            "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
            "Content-Type: application/json\r\n"
            "Connection: close\r\n"
            "\r\n"sv;

        boost::asio::async_write(*socket, boost::asio::buffer(HTTP_RESPONSE_HEADER.cbegin(), HTTP_RESPONSE_HEADER.length()),
            [this, reference = std::weak_ptr<HttpHeaderReader>(this->shared_from_this()), response = response] (const boost::system::error_code &e, std::size_t bytesSent) {
                if (reference.expired()) {
                    return;
                }

                if (e) {
                    if (server.verboseLogging) {
                        std::cerr << "status HTTP header write error: " << e.message() << std::endl;
                    }

                    removeFromServer();
                    return;
                }

                boost::asio::async_write(*socket, boost::asio::buffer(response->c_str(), response->length()),
                    [this, reference = std::weak_ptr<HttpHeaderReader>(this->shared_from_this()), response = response] (const boost::system::error_code &e, std::size_t bytesSent) {
                        if (reference.expired()) {
                            return;
                        }

                        if (e && server.verboseLogging) {
                            std::cerr << "status HTTP body write error: " << e.message() << std::endl;
                        }

                        removeFromServer();
                    });
            });*/
    }

    void handleRequest(typename SimpleHttpServer<SendHttpResponses>::HttpRequest request) {
        std::cout << "REQUEST " << request.getUri() << std::endl;
        std::cout << request.getHeaderFields() << std::endl;

        static constexpr auto response =
            "HTTP/1.1 200 OK\r\n"
            "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: close\r\n"
            "\r\nHello!"sv;

        boost::asio::async_write(*socket, boost::asio::buffer(response.cbegin(), response.length()),
            [requestCapture = std::move(request)] (const boost::system::error_code &e, std::size_t /*bytesSent*/) {
            });
    }

    UdpServer<Allocator> udpServer;
    SimpleHttpServer<SendHttpResponses> httpServer;

    bool verboseLogging = true;
    bool enableStatus = false;
};

typedef BasicServer<std::allocator<uint8_t>, true> Server;

#undef UDPPROXY_SERVER_NAME_DEFINE

}
