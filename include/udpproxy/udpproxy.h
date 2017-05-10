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

template <typename Allocator, bool SendHttpResponses>
class BasicServer {
public:
    BasicServer(boost::asio::io_service &ioService, const tcp::endpoint &endpoint)
            : acceptor(ioService, endpoint), udpServer(*this), clientsReadTimer(ioService) {}

    void runAsync() {
        static constexpr size_t CLIENT_READ_BUFFER_SIZE = 1024;
        clientsReadBuffer = std::make_shared<std::vector<uint8_t, InputBuffersAllocator>>(CLIENT_READ_BUFFER_SIZE);

        startAccept();
        readClients();
    }

    void setMaxHttpHeaderSize(size_t value) { maxHttpHeaderSize = value; }
    size_t getMaxHttpHeaderSize() { return maxHttpHeaderSize; }
    void setHttpConnectionTimeout(boost::asio::system_timer::duration value) { httpConnectionTimeout = value; }
    boost::asio::system_timer::duration getHttpConnectionTimeout() { return httpConnectionTimeout; }
    void setMaxUdpDataSize(size_t value) { maxUdpDataSize = value; }
    size_t getMaxUdpDataSize() { return maxUdpDataSize; }
    void setMaxOutputQueueLength(size_t value) { maxOutputQueueLength = value; }
    size_t getMaxOutputQueueLength() { return maxOutputQueueLength; }
    void setMaxHttpClients(size_t value) { maxHttpClients = value; }
    size_t getMaxHttpClients() { return maxHttpClients; };
    void setOutputQueueOverflowAlgorithm(OutputQueueOverflowAlgorithm value) { overflowAlgorithm = value; }
    OutputQueueOverflowAlgorithm getOutputQueueOverflowAlgorithm() { return overflowAlgorithm; };
    void setVerboseLogging(bool value) { verboseLogging = value; }
    bool getVerboseLogging() { return verboseLogging; }
    void setEnableStatus(bool value) { enableStatus = value; }
    bool getEnableStatus() { return enableStatus; }
    void setRenewMulticastSubscriptionInterval(boost::asio::system_timer::duration value) { renewMulticastSubscriptionInterval = value; }
    boost::asio::system_timer::duration getRenewMulticastSubscriptionInterval() { return renewMulticastSubscriptionInterval; }
    void setMulticastInterfaceAddress(boost::asio::ip::address value) { multicastInterfaceAddress = value; }
    boost::asio::ip::address getMulticastInterfaceAddress() { return multicastInterfaceAddress; }

private:
    typedef typename std::allocator_traits<Allocator>::template rebind_alloc<std::vector<uint8_t>> InputBuffersAllocator;

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

    void readClients() {
        // Slowly read clients' sockets to detect disconnected ones

        static constexpr boost::asio::system_timer::duration CLIENT_READ_PERIOD = 5s;

        clientsReadTimer.expires_from_now(CLIENT_READ_PERIOD);
        clientsReadTimer.async_wait([this] (const boost::system::error_code &e) {
            if (e == boost::system::errc::operation_canceled) {
                return;
            }

            for (auto& udpInput : udpServer.udpInputs) {
                for (auto& client : udpInput.second->clients) {
                    client->doReadCheck();
                }
            }

            readClients();
        });
    }

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

            if ((maxHttpClients == 0) || (clientsCounter <= maxHttpClients)) {
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
                    "Content-Length: 33"
                    "\r\n"
                    "Maximum number of clients reached");
            }

            startAccept();
        });
    }

    struct UdpServer {
        UdpServer(BasicServer &server) noexcept: server(server) {}

        void addUdpToHttpClient(std::shared_ptr<tcp::socket> &clientSocket, const boost::asio::ip::udp::endpoint &udpEndpoint) {
            uint64_t inputId = getEndpointId(udpEndpoint);

            auto udpInputIterator = udpInputs.find(inputId);
            UdpInput *udpInput;

            if (udpInputIterator == udpInputs.end()) {
                std::unique_ptr<UdpInput> udpInputUnique;

                udpInputUnique = std::make_unique<UdpInput>(server, inputId, udpEndpoint);

                udpInput = udpInputUnique.get();
                udpInputs.emplace(inputId, std::move(udpInputUnique));
            } else {
                udpInput = udpInputIterator->second.get();
            }

            udpInput->addClient(clientSocket);
        }

        struct UdpInput : public std::enable_shared_from_this<UdpServer::UdpInput> {
            UdpInput(BasicServer &server, uint64_t id, const boost::asio::ip::udp::endpoint &udpEndpoint)
                    : server(server), id(id), udpSocket(server.acceptor.get_io_service()),
                      udpEndpoint(udpEndpoint), renewMulticastSubscriptionTimer(server.acceptor.get_io_service()) {
                try {
                    udpSocket.open(udpEndpoint.protocol());
                    udpSocket.set_option(boost::asio::ip::udp::socket::reuse_address(true)); // FIXME: is it good?
                    udpSocket.bind(udpEndpoint);

                    if (udpEndpoint.address().is_multicast()) {
                        udpSocket.set_option(boost::asio::ip::multicast::join_group(udpEndpoint.address().to_v4(), server.multicastInterfaceAddress.to_v4()));
                    }
                } catch (const boost::system::system_error &e) {
                    throw ServerError(e.what(),
                        "HTTP/1.1 500 Internal Server Error\r\n"
                        "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                        "Content-Type: text/plain\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "UDP socket error");
                }

                if (server.verboseLogging) {
                    std::cerr << "new UDP input: udp://" << udpEndpoint << std::endl;
                }
            }

            ~UdpInput() {
                if (server.verboseLogging) {
                    std::cerr << "remove UDP input: " << udpEndpoint << std::endl;
                }
            }

            void startRenewMulticastSubscription() {
                renewMulticastSubscriptionTimer.expires_from_now(server.renewMulticastSubscriptionInterval);
                renewMulticastSubscriptionTimer.async_wait([this, reference = std::weak_ptr<UdpInput>(this->shared_from_this())] (const boost::system::error_code &e) {
                    if (reference.expired()) {
                        return;
                    } else if (e) {
                        return;
                    }

                    try {
                        if (server.verboseLogging) {
                            std::cerr << "renew multicast subscription for " << udpEndpoint << std::endl;
                        }
                        udpSocket.set_option(boost::asio::ip::multicast::leave_group(udpEndpoint.address()));
                        udpSocket.set_option(boost::asio::ip::multicast::join_group(udpEndpoint.address()));
                    } catch (const boost::system::system_error &e) {
                        std::cerr << "error: failed to renew multicast subscription for " << udpEndpoint << ": " << e.what() << std::endl;
                        server.udpServer.udpInputs.erase(id);
                        return;
                    }

                    startRenewMulticastSubscription();
                });
            }

            void start() {
                if (!isStarted) {
                    isStarted = true;

                    if (udpEndpoint.address().is_multicast() && (server.renewMulticastSubscriptionInterval != 0s)) {
                        startRenewMulticastSubscription();
                    }

                    receiveUdp();
                }
            }

            void receiveUdp() {
                if (server.enableStatus) {
                    static constexpr std::chrono::system_clock::duration BITRATE_PERIOD = 5s;

                    auto now = std::chrono::system_clock::now();

                    if (bitrateCalculationStart == std::chrono::system_clock::time_point::min()) {
                        bitrateCalculationStart = now;
                    } else {
                        auto duration = now - bitrateCalculationStart;
                        if (duration >= BITRATE_PERIOD) {
                            inBitrateKbit = 8. * bytesIn / std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
                            bytesIn = 0;
                            bitrateCalculationStart = now;
                        }
                    }
                }

                inputBuffer = std::make_shared<std::vector<uint8_t, InputBuffersAllocator>>(server.maxUdpDataSize);

                udpSocket.async_receive_from(boost::asio::buffer(inputBuffer->data(), inputBuffer->size()), senderEndpoint,
                    [this, reference = std::weak_ptr<UdpInput>(this->shared_from_this()), buffer = inputBuffer] (const boost::system::error_code &e, std::size_t bytesRead) {
                        if (reference.expired()) {
                            return;
                        }

                        if (e) {
                            std::cerr << "UDP socket receive error for " << udpEndpoint << ": " << e.message() << std::endl;
                            server.udpServer.udpInputs.erase(id);
                            return;
                        }

                        bytesIn += bytesRead;

                        inputBuffer->resize(bytesRead);

                        for (auto& client : clients) {
                            size_t length = client->outputBuffers.size();

                            if (length == 0) {
                                client->outputBuffers.emplace_back(inputBuffer);
                                client->writeData(inputBuffer);
                            } else if ((server.maxOutputQueueLength == 0) || (length < server.maxOutputQueueLength)) {
                                client->outputBuffers.emplace_back(inputBuffer);
                            } else {
                                switch (server.overflowAlgorithm) {
                                case OutputQueueOverflowAlgorithm::ClearQueue:
                                    if (server.verboseLogging) {
                                        std::cerr << "error: output queue overflow - clearing queue for " << client->remoteEndpoint << " (udp://" << udpEndpoint << ")" << std::endl;
                                    }
                                    client->outputBuffers.resize(1);
                                    break;

                                case OutputQueueOverflowAlgorithm::DropData:
                                    if (server.verboseLogging) {
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
                auto client = std::make_shared<typename UdpInput::Client>(socket, server, id);
                clients.emplace_back(client);
                client->writeHttpHeader();
            }

            struct Client : public std::enable_shared_from_this<UdpInput::Client> {
                Client(std::shared_ptr<tcp::socket> &socket, BasicServer &server, uint64_t inputId) noexcept
                        : socket(socket), server(server), inputId(inputId), remoteEndpoint(socket->remote_endpoint()) {
                    if (server.verboseLogging) {
                        std::cerr << "new HTTP client: " << remoteEndpoint << std::endl;
                    }
                    server.clientsCounter++;
                }

                ~Client() noexcept {
                    if (server.verboseLogging) {
                        std::cerr << "remove HTTP client: " << remoteEndpoint << std::endl;
                    }
                    server.clientsCounter--;
                }

                void writeData(std::shared_ptr<std::vector<uint8_t, InputBuffersAllocator>> &buffer) {
                    if (server.enableStatus) {
                        static constexpr std::chrono::system_clock::duration BITRATE_PERIOD = 5s;

                        auto now = std::chrono::system_clock::now();

                        if (bitrateCalculationStart == std::chrono::system_clock::time_point::min()) {
                            bitrateCalculationStart = now;
                        } else {
                            auto duration = now - bitrateCalculationStart;
                            if (duration >= BITRATE_PERIOD) {
                                outBitrateKbit = 8. * bytesOut / std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
                                bytesOut = 0;
                                bitrateCalculationStart = now;
                            }
                        }
                    }

                    boost::asio::async_write(*socket, boost::asio::buffer(buffer->data(), buffer->size()),
                        [this, reference = std::weak_ptr<Client>(this->shared_from_this()), buffer = buffer] (const boost::system::error_code &e, std::size_t bytesSent) {
                            if (reference.expired()) {
                                return;
                            }

                            if (e) {
                                if (server.verboseLogging) {
                                    std::cerr << "HTTP write error for " << remoteEndpoint << ": " << e.message() << std::endl;
                                }
                                server.udpServer.removeUdpToHttpClient(inputId, socket);
                                return;
                            }

                            bytesOut += bytesSent;

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
                    socket->async_read_some(boost::asio::buffer(server.clientsReadBuffer->data(), server.clientsReadBuffer->size()),
                        [this, reference = std::weak_ptr<Client>(this->shared_from_this()), buffer = server.clientsReadBuffer] (const boost::system::error_code &e, std::size_t /*bytesRead*/) mutable {
                            if (reference.expired()) {
                                return;
                            }

                            if (!e) {
                                return;
                            } else if (server.verboseLogging) {
                                std::cerr << "error reading client " << remoteEndpoint << ": " << e.message() << std::endl;
                            }

                            server.udpServer.removeUdpToHttpClient(inputId, socket);
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
                                if (server.verboseLogging) {
                                    std::cerr << "HTTP header write error for " << remoteEndpoint << ": " << e.message() << std::endl;
                                }

                                server.udpServer.removeUdpToHttpClient(inputId, socket);
                                return;
                            }

                            auto udpInputIterator = server.udpServer.udpInputs.find(inputId);
                            assert(udpInputIterator != server.udpServer.udpInputs.end());
                            udpInputIterator->second->start();
                        });
                }

                std::shared_ptr<tcp::socket> socket;
                BasicServer &server;
                uint64_t inputId;
                boost::asio::ip::tcp::endpoint remoteEndpoint;
                std::list<std::shared_ptr<std::vector<uint8_t, InputBuffersAllocator>>> outputBuffers;
                std::chrono::system_clock::time_point bitrateCalculationStart = std::chrono::system_clock::time_point::min();
                size_t bytesOut = 0;
                double outBitrateKbit = 0;
                bool readSomeDone = true;
            };

            BasicServer &server;
            uint64_t id;
            std::list<std::shared_ptr<UdpInput::Client>> clients;
            udp::socket udpSocket;
            udp::endpoint senderEndpoint;
            udp::endpoint udpEndpoint;
            std::shared_ptr<std::vector<uint8_t, InputBuffersAllocator>> inputBuffer;
            bool isStarted = false;
            std::chrono::system_clock::time_point bitrateCalculationStart = std::chrono::system_clock::time_point::min();
            size_t bytesIn = 0;
            double inBitrateKbit = 0;
            boost::asio::system_timer renewMulticastSubscriptionTimer;
        };

        static uint64_t getEndpointId(const boost::asio::ip::udp::endpoint &udpEndpoint) {
            return (static_cast<uint64_t>(udpEndpoint.address().to_v4().to_ulong()) << 2) | udpEndpoint.port();
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

        std::unordered_map<uint64_t, std::shared_ptr<UdpInput>> udpInputs;
        BasicServer &server;
    };

    struct HttpHeaderReader : public std::enable_shared_from_this<HttpHeaderReader> {
        HttpHeaderReader(std::shared_ptr<tcp::socket> &socket, BasicServer &server)
                : socket(socket), buffer(std::make_shared<boost::asio::streambuf>(server.maxHttpHeaderSize)),
                  timeoutTimer(socket->get_io_service()), server(server) {
            server.clientsCounter++;
        }

        ~HttpHeaderReader() noexcept {
            server.clientsCounter--;
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
                [this, reference = std::weak_ptr<HttpHeaderReader>(this->shared_from_this()), buffer = buffer] (const boost::system::error_code &e, size_t size) {
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
            static constexpr size_t MIN_UDP_REQUEST_LINE_SIZE = "/udp/d.d.d.d:d"sv.length() + HTTP_VERSION_ENDING.length();
            static constexpr std::experimental::string_view STATUS_URI = "/status"sv;

            boost::asio::async_read_until(*socket, *buffer,
                UntilFunction(MatchStringOrSize("\r\n", server.maxHttpHeaderSize - bytesRead - "\r\n"sv.length())),
                [this, reference = std::weak_ptr<HttpHeaderReader>(this->shared_from_this()), buffer = buffer] (const boost::system::error_code &e, size_t size) {
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
                                "505 HTTP Version Not Supported\r\n"
                                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                                "Content-Type: text/plain\r\n"
                                "Connection: close\r\n"
                                "\r\n"
                                "Only HTTP/1.1 is supported");
                        }

                        std::experimental::string_view uri = {boost::asio::buffer_cast<const char*>(buffer->data()), size - HTTP_VERSION_ENDING.length()};

                        if (server.enableStatus && (size == STATUS_URI.length() + HTTP_VERSION_ENDING.length())) {
                            if (uri == STATUS_URI) {
                                processStatus = true;
                                buffer->consume(size - 2); // Do not consume CRLF
                                bytesRead += (size - 2);
                                readRestOfHttpHeader();
                                return;
                            } else {
                                throw ServerError("wrong URI: " + std::string(uri),
                                    "HTTP/1.1 404 Not Found\r\n"
                                    "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                                    "Content-Type: text/plain\r\n"
                                    "Connection: close\r\n"
                                    "\r\n"
                                    "404 Not Found");
                            }
                        } else if (size < MIN_UDP_REQUEST_LINE_SIZE) {
                            throw ServerError("wrong URI: " + std::string(uri),
                                "HTTP/1.1 404 Not Found\r\n"
                                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                                "Content-Type: text/plain\r\n"
                                "Connection: close\r\n"
                                "\r\n"
                                "404 Not Found");
                        }

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

                        udpEndpoint = {address, static_cast<unsigned short>(port)};

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
            boost::asio::async_read_until(*socket, *buffer,
                UntilFunction(MatchStringOrSize("\r\n\r\n", server.maxHttpHeaderSize - bytesRead)),
                [this, reference = std::weak_ptr<HttpHeaderReader>(this->shared_from_this()), buffer = buffer] (const boost::system::error_code &e, size_t /*size*/) {
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

                    if (processStatus) {
                        this->buffer.reset();

                        if (server.verboseLogging) {
                            std::cerr << "status HTTP client: " << socket->remote_endpoint() << std::endl;
                        }

                        writeJsonStatus();
                    } else {
                        try {
                            server.udpServer.addUdpToHttpClient(socket, udpEndpoint);
                        } catch (const ServerError &e) {
                            std::cerr << "UDP socket error for " << udpEndpoint << ": " << e.what() << std::endl;
                            sendFinalHttpResponse(e.http_response());
                            return;
                        }

                        removeFromServer();
                    }
                });
        }

        void writeJsonStatus() {
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
                [this, reference = std::weak_ptr<HttpHeaderReader>(this->shared_from_this()), response = response] (const boost::system::error_code &e, std::size_t /*bytesSent*/) {
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
                        [this, reference = std::weak_ptr<HttpHeaderReader>(this->shared_from_this()), response = response] (const boost::system::error_code &e, std::size_t /*bytesSent*/) {
                            if (reference.expired()) {
                                return;
                            }

                            if (e && server.verboseLogging) {
                                std::cerr << "status HTTP body write error: " << e.message() << std::endl;
                            }

                            removeFromServer();
                        });
                });
        }

        std::shared_ptr<tcp::socket> socket;
        std::shared_ptr<boost::asio::streambuf> buffer;
        size_t bytesRead = 0;
        boost::asio::system_timer timeoutTimer;
        BasicServer &server;
        boost::asio::ip::udp::endpoint udpEndpoint;
        bool processStatus = false;
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
    UdpServer udpServer;
    std::list<std::shared_ptr<HttpHeaderReader>> httpHeaderReaders;
    size_t clientsCounter = 0;
    std::shared_ptr<std::vector<uint8_t, InputBuffersAllocator>> clientsReadBuffer;
    boost::asio::system_timer clientsReadTimer;

    size_t maxHttpHeaderSize = 4 * 1024;
    boost::asio::system_timer::duration httpConnectionTimeout = 1s;
    size_t maxUdpDataSize = 4 * 1024;
    size_t maxOutputQueueLength = 1024;
    size_t maxHttpClients = 0;
    OutputQueueOverflowAlgorithm overflowAlgorithm = OutputQueueOverflowAlgorithm::ClearQueue;
    bool verboseLogging = true;
    bool enableStatus = false;
    boost::asio::system_timer::duration renewMulticastSubscriptionInterval = 0s;
    boost::asio::ip::address multicastInterfaceAddress;
};

typedef BasicServer<std::allocator<uint8_t>, true> Server;

#undef UDPPROXY_SERVER_NAME_DEFINE

}
