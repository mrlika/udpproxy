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

enum class OutputQueueOverflowAlgorithm {
    ClearQueue,
    DropData
};

template <typename Allocator>
class BasicServer {
public:
    BasicServer(boost::asio::io_service &ioService, const tcp::endpoint &endpoint)
            : acceptor(ioService, endpoint), udpServer(*this), clientsReadTimer(ioService) {
        static constexpr size_t CLIENT_READ_BUFFER_SIZE = 1024;

        clientsReadBuffer = std::make_shared<std::vector<uint8_t, InputBuffersAllocator>>(CLIENT_READ_BUFFER_SIZE);

        startAccept();
        readClients();
    }

    void setMaxHeaderSize(size_t value) { maxHeaderSize = value; }
    size_t getMaxHeaderSize() { return maxHeaderSize; }
    void setHeaderReadTimeout(boost::asio::system_timer::duration value) { headerReadTimeout = value; }
    boost::asio::system_timer::duration getHeaderReadTimeout() { return headerReadTimeout; }
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

private:
    typedef typename std::allocator_traits<Allocator>::template rebind_alloc<std::vector<uint8_t>> InputBuffersAllocator;

    typedef boost::asio::buffers_iterator<boost::asio::streambuf::const_buffers_type> UntilIterator;
    typedef std::function<std::pair<UntilIterator, bool>(UntilIterator begin, UntilIterator end) noexcept> UntilFunction;

    class ServerError : public std::runtime_error {
    public:
        explicit ServerError(const std::string& message) : std::runtime_error(message) {}
        explicit ServerError(const char *message) : std::runtime_error(message) {}
    };

    void readClients() {
        // Slowly read clients' sockets to detect disconnected ones

        static constexpr boost::asio::system_timer::duration CLIENT_READ_PERIOD = 5s;

        clientsReadTimer.expires_from_now(CLIENT_READ_PERIOD);
        clientsReadTimer.async_wait([this] (const boost::system::error_code &/*e*/) {
            for (auto& udpInput : udpServer.udpInputs) {
                for (auto& client : udpInput.second->clients) {
                    if (!client->readSomeDone) {
                        continue;
                    }

                    client->readSomeDone = false;
                    client->socket->async_read_some(boost::asio::buffer(clientsReadBuffer->data(), clientsReadBuffer->size()),
                        [this, client = client] (const boost::system::error_code &e, std::size_t /*bytesRead*/) mutable {
                            if (e) {
                                if (verboseLogging) {
                                    std::cerr << "error reading client " << client->remoteEndpoint << ": " << e.message() << std::endl;
                                }
                                udpServer.removeUdpToHttpClient(client->inputId, client->socket);
                            }
                        });
                }
            }

            readClients();
        });
    }

    void startAccept() {
        auto socket = std::make_shared<tcp::socket>(acceptor.get_io_service());

        acceptor.async_accept(*socket, [this, socket] (const boost::system::error_code &e) mutable {
            if (!e) {
                if ((maxHttpClients == 0) || (clientsCounter < maxHttpClients)) {
                    HttpHeaderReader::read(socket, *this);
                } else if (verboseLogging) {
                    std::cerr << "Maximum of HTTP clients reached. Connection refused: " << socket->remote_endpoint() << std::endl;
                }
            } else {
                std::cerr << "TCP accept error: " << e.message() << std::endl;
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

                try {
                    udpInputUnique = std::make_unique<UdpInput>(server, inputId, udpEndpoint);
                } catch (const ServerError &e) {
                    std::cerr << "UDP socket setup error: " << e.what() << std::endl;
                    return;
                }

                udpInput = udpInputUnique.get();
                udpInputs.emplace(inputId, std::move(udpInputUnique));
            } else {
                udpInput = udpInputIterator->second.get();
            }

            static constexpr std::experimental::string_view HTTP_RESPONSE_HEADER =
                "HTTP/1.1 200 OK\r\n"
                "Server: udp-proxy\r\n"
                "Content-Type: application/octet-stream\r\n"
                "\r\n"sv;

            boost::asio::async_write(*clientSocket, boost::asio::buffer(HTTP_RESPONSE_HEADER.cbegin(), HTTP_RESPONSE_HEADER.length()),
                [this, clientSocket = clientSocket, inputId] (const boost::system::error_code &e, std::size_t /*bytesSent*/) {
                    if (e) {
                        if (server.verboseLogging) {
                            std::cerr << "HTTP header write error: " << e.message() << std::endl;
                        }
                        removeUdpToHttpClient(inputId, clientSocket);
                        return;
                    }

                    auto udpInputIterator = udpInputs.find(inputId);
                    if (udpInputIterator != udpInputs.end()) {
                        udpInputIterator->second->start();
                    }
                });

            udpInput->clients.emplace_back(std::make_shared<typename UdpInput::Client>(clientSocket, server, inputId));
        }

        struct UdpInput : public std::enable_shared_from_this<UdpServer::UdpInput> {
            UdpInput(BasicServer &server, uint64_t id, const boost::asio::ip::udp::endpoint &udpEndpoint)
                    : server(server), id(id), udpSocket(server.acceptor.get_io_service()),
                      udpEndpoint(udpEndpoint) {
                udpSocket.open(udpEndpoint.protocol());

                try {
                    udpSocket.bind(udpEndpoint);
                } catch (const boost::system::system_error &e) {
                    throw ServerError(e.what());
                }

                udpSocket.set_option(boost::asio::ip::udp::socket::reuse_address(true)); // FIXME: is it good?
                if (udpEndpoint.address().is_multicast()) {
                    udpSocket.set_option(boost::asio::ip::multicast::join_group(udpEndpoint.address()));
                }

                if (server.verboseLogging) {
                    std::cerr << "new UDP input: udp://" << udpSocket.local_endpoint() << std::endl;
                }
            }

            ~UdpInput() {
                if (server.verboseLogging) {
                    std::cerr << "remove UDP input: " << udpEndpoint << std::endl;
                }
            }

            void start() {
                if (!isStarted) {
                    isStarted = true;
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
                    [this, capture = this->shared_from_this()] (const boost::system::error_code &e, std::size_t bytesRead) {
                        if (e) {
                            std::cerr << "UDP socket receive error: " << e.message() << std::endl;
                            server.udpServer.removeUdpInput(id);
                            return;
                        }

                        bytesIn += bytesRead;

                        inputBuffer->resize(bytesRead);

                        for (auto& client : clients) {
                            size_t length = client->outputBuffers.size();

                            if (length == 0) {
                                client->outputBuffers.emplace_back(inputBuffer);
                                client->write(*inputBuffer);
                            } else if ((server.maxOutputQueueLength != 0) && (length >= server.maxOutputQueueLength)) {
                                switch (server.overflowAlgorithm) {
                                case OutputQueueOverflowAlgorithm::ClearQueue:
                                    if (server.verboseLogging) {
                                        std::cerr << "error: output queue overflow - clearing queue for " << client->remoteEndpoint << std::endl;
                                    }
                                    client->outputBuffers.resize(1);
                                    break;

                                case OutputQueueOverflowAlgorithm::DropData:
                                    if (server.verboseLogging) {
                                        std::cerr << "error: output queue overflow - dropping data for " << client->remoteEndpoint << std::endl;
                                    }
                                    break;
                                }
                            }
                        }

                        receiveUdp();
                    });
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

                void write(std::vector<uint8_t, InputBuffersAllocator> &buffer) {
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

                    boost::asio::async_write(*socket, boost::asio::buffer(buffer.data(), buffer.size()),
                        [this, capture = this->shared_from_this(), bufferPointer = buffer.data()] (const boost::system::error_code &e, std::size_t bytesSent) {
                            if (e) {
                                if (server.verboseLogging) {
                                    std::cerr << "HTTP write error: " << e.message() << std::endl;
                                }
                                server.udpServer.removeUdpToHttpClient(inputId, socket);
                                return;
                            }

                            bytesOut += bytesSent;

                            assert(bufferPointer == outputBuffers.front()->data());
                            (void)bytesSent; // Avoid unused parameter warning when asserts disabled
                            assert(bytesSent == outputBuffers.front()->size());

                            outputBuffers.pop_front();
                            if (!outputBuffers.empty()) {
                                write(*outputBuffers.front());
                            }
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
        };

        static uint64_t getEndpointId(const boost::asio::ip::udp::endpoint &udpEndpoint) {
            return (static_cast<uint64_t>(udpEndpoint.address().to_v4().to_ulong()) << 2) | udpEndpoint.port();
        }

        void removeUdpToHttpClient(uint64_t inputId, const std::shared_ptr<tcp::socket> &clientSocket) {
            auto udpInputIterator = udpInputs.find(inputId);

            if (udpInputIterator == udpInputs.end()) {
                return;
            }

            removeUdpToHttpClient(udpInputIterator, clientSocket);
        }

        void removeUdpToHttpClient(typename std::unordered_map<uint32_t, std::shared_ptr<UdpInput>>::iterator udpInputIterator, const std::shared_ptr<tcp::socket> &clientSocket) {
            auto& clients = udpInputIterator->second->clients;
            auto clientIterator = std::find_if(clients.begin(), clients.end(), [&clientSocket] (const std::shared_ptr<typename UdpInput::Client> &client) { return client->socket == clientSocket; });
            removeUdpToHttpClient(udpInputIterator, clientIterator);
        }

        void removeUdpToHttpClient(typename std::unordered_map<uint32_t, std::shared_ptr<UdpInput>>::iterator udpInputIterator, typename std::list<std::shared_ptr<typename UdpInput::Client>>::iterator clientIterator) {
            auto& clients = udpInputIterator->second->clients;

            if (clientIterator != clients.end()) {
                auto& client = *clientIterator;
                client->socket->cancel();
                client->socket->close();
                clients.erase(clientIterator);
            }

            if (clients.empty()) {
                udpInputIterator->second->udpSocket.cancel();
                udpInputIterator->second->udpSocket.close();
                udpInputs.erase(udpInputIterator);
            }
        }

        void removeUdpInput(uint64_t inputId) {
            auto udpInputIterator = udpInputs.find(inputId);

            if (udpInputIterator == udpInputs.end()) {
                return;
            }

            for (auto& client : udpInputIterator->second->clients) {
                client->socket->cancel();
                client->socket->close();
            }

            udpInputIterator->second->udpSocket.cancel();
            udpInputIterator->second->udpSocket.close();
            udpInputs.erase(udpInputIterator);
        }

        std::unordered_map<uint32_t, std::shared_ptr<UdpInput>> udpInputs;
        BasicServer &server;
    };

    class HttpHeaderReader : public std::enable_shared_from_this<HttpHeaderReader> {
    public:
        static void read(std::shared_ptr<tcp::socket> &socket, BasicServer &server) {
            auto reader = std::make_shared<HttpHeaderReader>(socket, server);
            reader->validateHttpMethod();
        }

        HttpHeaderReader(std::shared_ptr<tcp::socket> &socket, BasicServer &server)
                : socket(socket), buffer(server.maxHeaderSize),
                  timeoutTimer(socket->get_io_service()), server(server) {
            server.clientsCounter++;
        }

        ~HttpHeaderReader() noexcept {
            server.clientsCounter--;
        }

    private:
        void validateHttpMethod() {
            static constexpr std::experimental::string_view REQUEST_METHOD = "GET "sv;

            timeoutTimer.expires_from_now(server.headerReadTimeout);
            timeoutTimer.async_wait([this, capture = this->shared_from_this()] (const boost::system::error_code &e) {
                if (e != boost::system::errc::operation_canceled) {
                    socket->cancel();
                }
            });

            boost::asio::async_read_until(*socket, buffer,
                UntilFunction(MatchStringOrSize(REQUEST_METHOD, REQUEST_METHOD.length())),
                [this, capture = this->shared_from_this()] (const boost::system::error_code &e, size_t size) {
                    try {
                        if (e) {
                            throw ServerError(e.message());
                        }

                        std::experimental::string_view method(boost::asio::buffer_cast<const char*>(buffer.data()), REQUEST_METHOD.length());
                        if (REQUEST_METHOD != method) {
                            throw ServerError("method not supported");
                        }

                        buffer.consume(size);
                        bytesRead += size;
                        readHttpRequestUri();
                    } catch (const ServerError &e) {
                        if (server.verboseLogging) {
                            std::cerr << "HTTP client error: " << e.what() << std::endl;
                        }
                        timeoutTimer.cancel();
                    }
                });
        }

        void readHttpRequestUri() {
            static constexpr std::experimental::string_view HTTP_VERSION_ENDING = " HTTP/1.1\r\n"sv;
            static constexpr size_t MIN_UDP_REQUEST_LINE_SIZE = "/udp/d.d.d.d:d"sv.length() + HTTP_VERSION_ENDING.length();
            static constexpr std::experimental::string_view STATUS_URI = "/status"sv;

            boost::asio::async_read_until(*socket, buffer,
                UntilFunction(MatchStringOrSize("\r\n", server.maxHeaderSize - bytesRead - "\r\n"sv.length())),
                [this, capture = this->shared_from_this()] (const boost::system::error_code &e, size_t size) {
                    try {
                        if (e) {
                            throw ServerError(e.message());
                        } else if (size <= HTTP_VERSION_ENDING.length()) {
                            throw ServerError("request not supported");
                        }

                        std::experimental::string_view ending = {boost::asio::buffer_cast<const char*>(buffer.data()) + size - HTTP_VERSION_ENDING.length(), HTTP_VERSION_ENDING.length()};
                        if (HTTP_VERSION_ENDING != ending) {
                            throw ServerError("request not supported");
                        }

                        std::experimental::string_view uri = {boost::asio::buffer_cast<const char*>(buffer.data()), size - HTTP_VERSION_ENDING.length()};

                        if (server.enableStatus && (size == STATUS_URI.length() + HTTP_VERSION_ENDING.length())) {
                            std::experimental::string_view ending = {boost::asio::buffer_cast<const char*>(buffer.data()) + size - HTTP_VERSION_ENDING.length(), HTTP_VERSION_ENDING.length()};
                            if (HTTP_VERSION_ENDING != ending) {
                                throw ServerError("request not supported");
                            }

                            std::experimental::string_view uri = {boost::asio::buffer_cast<const char*>(buffer.data()), size - HTTP_VERSION_ENDING.length()};
                            if (uri == STATUS_URI) {
                                processStatus = true;
                                buffer.consume(size - 2); // Do not consume CRLF
                                bytesRead += (size - 2);
                                readRestOfHttpHeader();
                                return;
                            } else {
                                throw ServerError("request not supported");
                            }
                        } else if (size < MIN_UDP_REQUEST_LINE_SIZE) {
                            throw ServerError("request not supported");
                        }

                        // TODO: replace regex with parsing algorithm for better preformance and to avoid memory allocations
                        static const std::regex uriRegex("/udp/(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}):(\\d{1,5})(?:\\?.*)?", std::regex_constants::optimize);
                        std::cmatch match;
                        std::regex_match(uri.begin(), uri.end(), match, uriRegex);

                        if (match.empty()) {
                            throw ServerError("wrong URI: " + std::string(uri));
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
                            throw ServerError("wrong URI");
                        }

                        if ((port == 0) || (port > std::numeric_limits<uint16_t>::max())) {
                            throw ServerError("wrong URI");
                        }

                        udpEndpoint = {address, static_cast<unsigned short>(port)};

                        buffer.consume(size - 2); // Do not consume CRLF
                        bytesRead += (size - 2);
                        readRestOfHttpHeader();
                    } catch (const ServerError &e) {
                        if (server.verboseLogging) {
                            std::cerr << "HTTP client error: " << e.what() << std::endl;
                        }
                        timeoutTimer.cancel();
                    }
                });
        }

        void readRestOfHttpHeader() {
            boost::asio::async_read_until(*socket, buffer,
                UntilFunction(MatchStringOrSize("\r\n\r\n", server.maxHeaderSize - bytesRead)),
                [this, capture = this->shared_from_this()] (const boost::system::error_code &e, size_t /*size*/) {
                    timeoutTimer.cancel();

                    if (e) {
                        if (server.verboseLogging) {
                            std::cerr << "HTTP client error: " << e.message() << std::endl;
                        }
                        return;
                    }

                    if (processStatus) {
                        if (server.verboseLogging) {
                            std::cerr << "status HTTP client: " << socket->remote_endpoint() << std::endl;
                        }

                        static constexpr std::experimental::string_view HTTP_RESPONSE_HEADER =
                            "HTTP/1.1 200 OK\r\n"
                            "Server: udp-proxy\r\n"
                            "Content-Type: application/json\r\n"
                            "\r\n"sv;

                        boost::asio::async_write(*socket, boost::asio::buffer(HTTP_RESPONSE_HEADER.cbegin(), HTTP_RESPONSE_HEADER.length()),
                            [this, capture = this->shared_from_this()] (const boost::system::error_code &e, std::size_t /*bytesSent*/) {
                                if (e) {
                                    if (server.verboseLogging) {
                                        std::cerr << "status HTTP header write error: " << e.message() << std::endl;
                                    }
                                    return;
                                }

                                writeJsonStatus();
                            });
                    } else {
                        server.udpServer.addUdpToHttpClient(socket, udpEndpoint);
                    }
                });
        }

        void writeJsonStatus() {
            // TODO: optimize JSON output
            std::ostringstream out;

            out << "{\"inputs\":[";
            bool first = true;
            for (const auto& udpInputItem : server.udpServer.udpInputs) {
                if (first) {
                    first = false;
                    out << '{';
                } else {
                    out << ",{";
                }

                const auto& udpInput = *(udpInputItem.second);
                out << "\"endpoint\":\"" << udpInput.udpEndpoint << "\",";
                out << "\"clients_count\":" << udpInput.clients.size() << ',';
                out << "\"bitrate\":" << udpInput.inBitrateKbit << '}';
            }
            out << "],\"clients\":[";
            first = true;
            for (const auto& udpInput : server.udpServer.udpInputs) {
                std::string udpEndpointJson = "\"udp_endpoint\":\"" + boost::lexical_cast<std::string>(udpInput.second->udpEndpoint) + "\",";
                for (const auto& client : udpInput.second->clients) {
                    if (first) {
                        first = false;
                        out << '{';
                    } else {
                        out << ",{";
                    }

                    out << "\"remote_endpoint\":\"" << client->remoteEndpoint << "\",";
                    out << udpEndpointJson;
                    out << "\"output_queue_length\":" << client->outputBuffers.size() << ',';
                    out << "\"bitrate\":" << client->outBitrateKbit << '}';
                }
            }
            out << "]}";

            // TODO: optimize to use direct buffer access without copying
            auto response = std::make_shared<std::string>(out.str());

            boost::asio::async_write(*socket, boost::asio::buffer(response->c_str(), response->length()),
                [this, capture = this->shared_from_this(), response = response] (const boost::system::error_code &e, std::size_t /*bytesSent*/) {
                    if (e) {
                        if (server.verboseLogging) {
                            std::cerr << "status HTTP body write error: " << e.message() << std::endl;
                        }
                    }
                });
        }

        std::shared_ptr<tcp::socket> socket;
        boost::asio::streambuf buffer;
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
    size_t clientsCounter = 0;
    std::shared_ptr<std::vector<uint8_t, InputBuffersAllocator>> clientsReadBuffer;
    boost::asio::system_timer clientsReadTimer;

    size_t maxHeaderSize = 4 * 1024;
    boost::asio::system_timer::duration headerReadTimeout = 1s;
    size_t maxUdpDataSize = 4 * 1024;
    size_t maxOutputQueueLength = 1024;
    size_t maxHttpClients = 0;
    OutputQueueOverflowAlgorithm overflowAlgorithm = OutputQueueOverflowAlgorithm::ClearQueue;
    bool verboseLogging = true;
    bool enableStatus = false;
};

typedef BasicServer<std::allocator<uint8_t>> Server;

}
