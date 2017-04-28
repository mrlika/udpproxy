#pragma once

#include <boost/asio.hpp>
#include <boost/asio/system_timer.hpp>

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

// TODO: make this configurable
static constexpr size_t MAX_HEADER_SIZE = 4 * 1024;
static constexpr size_t MAX_UDP_DATAGRAM_SIZE = 4 * 1024;
static constexpr size_t MAX_WRITE_QUEUE_LENGTH = 1024;
static constexpr size_t MAX_HTTP_CLIENTS = 100;
static constexpr boost::asio::system_timer::duration HEADER_READ_TIMEOUT = 1s;
static constexpr bool VERBOSE_LOGGING = true;

class Server {
public:
    Server(boost::asio::io_service &ioService, const tcp::endpoint &endpoint)
            : acceptor(ioService, endpoint), udpServer(*this) {
        startAccept();
    }

private:
    typedef boost::asio::buffers_iterator<boost::asio::streambuf::const_buffers_type> UntilIterator;
    typedef std::function<std::pair<UntilIterator, bool>(UntilIterator begin, UntilIterator end) noexcept> UntilFunction;

    class ServerError : public std::runtime_error {
    public:
        explicit ServerError(const std::string& message) : std::runtime_error(message) {}
        explicit ServerError(const char *message) : std::runtime_error(message) {}
    };

    void startAccept() {
        auto socket = std::make_shared<tcp::socket>(acceptor.get_io_service());

        acceptor.async_accept(*socket, [this, socket] (const boost::system::error_code &e) mutable {
            if (!e) {
                if ((MAX_HTTP_CLIENTS != 0) && (clientsCounter < MAX_HTTP_CLIENTS)) {
                    HttpHeaderReader::read(socket, *this);
                }
            } else {
                std::cerr << "TCP accept error: " << e.message() << std::endl;
            }

            startAccept();
        });
    }

    class UdpServer {
    public:
        UdpServer(Server &server) noexcept: server(server) {}

        void addUdpToHttpReceiver(std::shared_ptr<tcp::socket> &receiverSocket, const boost::asio::ip::udp::endpoint &udpEndpoint) {
            uint64_t inputId = getEndpointId(udpEndpoint);

            auto udpInputIterator = udpInputs.find(inputId);
            UdpInput *udpInput;

            if (udpInputIterator == udpInputs.end()) {
                std::unique_ptr<UdpInput> udpInputUnique;

                try {
                    udpInputUnique = std::make_unique<UdpInput>(*this, inputId, udpEndpoint);
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

            boost::asio::async_write(*receiverSocket, boost::asio::buffer(HTTP_RESPONSE_HEADER.cbegin(), HTTP_RESPONSE_HEADER.length()),
                [this, receiverSocket = receiverSocket, inputId] (const boost::system::error_code &e, std::size_t /*bytesSent*/) {
                    if (e) {
                        if (VERBOSE_LOGGING) {
                            std::cerr << "HTTP header write error: " << e.message() << std::endl;
                        }
                        removeUdpToHttpReceiver(inputId, receiverSocket);
                        return;
                    }

                    auto udpInputIterator = udpInputs.find(inputId);
                    if (udpInputIterator != udpInputs.end()) {
                        udpInputIterator->second->start();
                    }
                });

            udpInput->receivers.emplace_back(std::make_shared<UdpInput::Receiver>(receiverSocket, server, inputId));
        }

    private:
        struct UdpInput : public std::enable_shared_from_this<UdpInput> {
            UdpInput(UdpServer &udpServer, uint64_t id, const boost::asio::ip::udp::endpoint &udpEndpoint)
                    : udpServer(udpServer), id(id), udpSocket(udpServer.server.acceptor.get_io_service()), udpEndpoint(udpEndpoint) {
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

                if (VERBOSE_LOGGING) {
                    std::cerr << "new UDP input: udp://" << udpSocket.local_endpoint().address() << ":" << udpSocket.local_endpoint().port() << std::endl;
                }
            }

            ~UdpInput() {
                if (VERBOSE_LOGGING) {
                    std::cerr << "remove UDP input: " << udpEndpoint.address() << ":" << udpEndpoint.port() << std::endl;
                }
            }

            void start() {
                if (!isStarted) {
                    isStarted = true;
                    receiveUdp();
                }
            }

            void receiveUdp() {
                inputBuffer = std::make_shared<std::vector<uint8_t>>(MAX_UDP_DATAGRAM_SIZE);

                udpSocket.async_receive_from(boost::asio::buffer(inputBuffer->data(), inputBuffer->size()), senderEndpoint,
                    [this, capture = shared_from_this()] (const boost::system::error_code &e, std::size_t bytesRead) {
                        if (e) {
                            std::cerr << "UDP socket receive error: " << e.message() << std::endl;
                            udpServer.removeUdpInput(id);
                            return;
                        }

                        inputBuffer->resize(bytesRead);

                        for (auto& receiver : receivers) {
                            size_t length = receiver->writeBuffers.size();

                            if (length == 0) {
                                receiver->write(inputBuffer);
                            } else if ((MAX_WRITE_QUEUE_LENGTH != 0) && (length >= MAX_WRITE_QUEUE_LENGTH)) {
                                std::cerr << "error: write queue limit reached - cleaning queue for " << receiver->remoteEndpoint.address() << ":" << receiver->remoteEndpoint.port() << std::endl;
                                receiver->writeBuffers.resize(1);
                            }

                            receiver->writeBuffers.emplace_back(inputBuffer);
                        }

                        receiveUdp();
                    });
            }

            struct Receiver : public std::enable_shared_from_this<Receiver> {
                Receiver(std::shared_ptr<tcp::socket> &socket, Server &server, uint64_t inputId) noexcept
                        : socket(socket), server(server), inputId(inputId), remoteEndpoint(socket->remote_endpoint()) {
                    if (VERBOSE_LOGGING) {
                        std::cerr << "new HTTP client: " << remoteEndpoint.address() << ":" << remoteEndpoint.port() << std::endl;
                    }
                    server.clientsCounter++;
                }

                ~Receiver() noexcept {
                    if (VERBOSE_LOGGING) {
                        std::cerr << "remove HTTP client: " << remoteEndpoint.address() << ":" << remoteEndpoint.port() << std::endl;
                    }
                    server.clientsCounter--;
                }

                void write(const std::shared_ptr<std::vector<uint8_t>> &buffer) {
                    boost::asio::async_write(*socket, boost::asio::buffer(buffer->data(), buffer->size()),
                        [this, capture = shared_from_this(), bufferPointer = buffer->data()] (const boost::system::error_code &e, std::size_t bytesSent) {
                            if (e) {
                                if (VERBOSE_LOGGING) {
                                    std::cerr << "HTTP write error: " << e.message() << std::endl;
                                }
                                server.udpServer.removeUdpToHttpReceiver(inputId, socket);
                                return;
                            }

                            assert(bufferPointer == writeBuffers.front()->data());
                            (void)bytesSent; // Avoid unused parameter warning when asserts disabled
                            assert(bytesSent == writeBuffers.front()->size());

                            writeBuffers.pop_front();
                            if (!writeBuffers.empty()) {
                                write(writeBuffers.front());
                            }
                        });
                }

                std::shared_ptr<tcp::socket> socket;
                Server &server;
                uint64_t inputId;
                boost::asio::ip::tcp::endpoint remoteEndpoint;
                std::list<std::shared_ptr<std::vector<uint8_t>>> writeBuffers;
            };

            UdpServer &udpServer;
            uint64_t id;
            std::list<std::shared_ptr<Receiver>> receivers;
            udp::socket udpSocket;
            udp::endpoint senderEndpoint;
            udp::endpoint udpEndpoint;
            std::shared_ptr<std::vector<uint8_t>> inputBuffer;
            bool isStarted = false;
        };

        static uint64_t getEndpointId(const boost::asio::ip::udp::endpoint &udpEndpoint) {
            return (static_cast<uint64_t>(udpEndpoint.address().to_v4().to_ulong()) << 2) | udpEndpoint.port();
        }

        void removeUdpToHttpReceiver(uint64_t inputId, const std::shared_ptr<tcp::socket> &receiverSocket) {
            auto udpInputIterator = udpInputs.find(inputId);

            if (udpInputIterator == udpInputs.end()) {
                return;
            }

            removeUdpToHttpReceiver(udpInputIterator, receiverSocket);
        }

        void removeUdpToHttpReceiver(std::unordered_map<uint32_t, std::shared_ptr<UdpInput>>::iterator udpInputIterator, const std::shared_ptr<tcp::socket> &receiverSocket) {
            auto& receivers = udpInputIterator->second->receivers;
            auto receiverIterator = std::find_if(receivers.begin(), receivers.end(), [&receiverSocket] (const std::shared_ptr<UdpInput::Receiver> &receiver) { return receiver->socket == receiverSocket; });
            removeUdpToHttpReceiver(udpInputIterator, receiverIterator);
        }

        void removeUdpToHttpReceiver(std::unordered_map<uint32_t, std::shared_ptr<UdpInput>>::iterator udpInputIterator, std::list<std::shared_ptr<UdpInput::Receiver>>::iterator receiverIterator) {
            auto& receivers = udpInputIterator->second->receivers;

            if (receiverIterator != receivers.end()) {
                auto& receiver = *receiverIterator;
                receiver->socket->cancel();
                receiver->socket->close();
                receivers.erase(receiverIterator);
            }

            if (receivers.empty()) {
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

            for (auto& receiver : udpInputIterator->second->receivers) {
                receiver->socket->cancel();
                receiver->socket->close();
            }

            udpInputIterator->second->udpSocket.cancel();
            udpInputIterator->second->udpSocket.close();
            udpInputs.erase(udpInputIterator);
        }

        std::unordered_map<uint32_t, std::shared_ptr<UdpInput>> udpInputs;
        Server &server;
    };

    class HttpHeaderReader : public std::enable_shared_from_this<HttpHeaderReader> {
    public:
        static void read(std::shared_ptr<tcp::socket> &socket, Server &server) {
            auto reader = std::make_shared<HttpHeaderReader>(socket, server);
            reader->validateHttpMethod();
        }

        HttpHeaderReader(std::shared_ptr<tcp::socket> &socket, Server &server)
                : socket(socket), timeoutTimer(socket->get_io_service()), server(server) {
            server.clientsCounter++;
        }

        ~HttpHeaderReader() noexcept {
            server.clientsCounter--;
        }

    private:
        void validateHttpMethod() {
            static constexpr std::experimental::string_view REQUEST_METHOD = "GET "sv;

            timeoutTimer.expires_from_now(HEADER_READ_TIMEOUT);
            timeoutTimer.async_wait([this, capture = shared_from_this()] (const boost::system::error_code &e) {
                if (e != boost::system::errc::operation_canceled) {
                    socket->cancel();
                }
            });

            boost::asio::async_read_until(*socket, buffer,
                UntilFunction(MatchStringOrSize(REQUEST_METHOD, REQUEST_METHOD.length())),
                [this, capture = shared_from_this()] (const boost::system::error_code &e, size_t size) {
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
                        if (VERBOSE_LOGGING) {
                            std::cerr << "HTTP client error: " << e.what() << std::endl;
                        }
                        timeoutTimer.cancel();
                    }
                });
        }

        void readHttpRequestUri() {
            static constexpr std::experimental::string_view HTTP_VERSION_ENDING = " HTTP/1.1\r\n"sv;
            static constexpr size_t MIN_REQUEST_LINE_SIZE = "/udp/d.d.d.d:d"sv.length() + HTTP_VERSION_ENDING.length();

            boost::asio::async_read_until(*socket, buffer,
                UntilFunction(MatchStringOrSize("\r\n", MAX_HEADER_SIZE - bytesRead - "\r\n"sv.length())),
                [this, capture = shared_from_this()] (const boost::system::error_code &e, size_t size) {
                    try {
                        if (e) {
                            throw ServerError(e.message());
                        } else if (size < MIN_REQUEST_LINE_SIZE) {
                            throw ServerError("request not supported");
                        }

                        std::experimental::string_view ending = {boost::asio::buffer_cast<const char*>(buffer.data()) + size - HTTP_VERSION_ENDING.length(), HTTP_VERSION_ENDING.length()};
                        if (HTTP_VERSION_ENDING != ending) {
                            throw ServerError("request not supported");
                        }

                        std::experimental::string_view uri = {boost::asio::buffer_cast<const char*>(buffer.data()), size - HTTP_VERSION_ENDING.length()};

                        // TODO: replace regex with parsing algorithm for better preformance and to avoid memory allocations
                        static const std::regex uriRegex("/udp/(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}):(\\d{1,5})(?:\\?.*)?", std::regex_constants::optimize);
                        std::cmatch match;
                        std::regex_match(uri.begin(), uri.end(), match, uriRegex);

                        if (match.empty()) {
                            throw ServerError("wrong URI");
                        }

                        boost::asio::ip::address address;
                        unsigned long port;

                        try {
                            port = std::stoul(match[2]);
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
                        if (VERBOSE_LOGGING) {
                            std::cerr << "HTTP client error: " << e.what() << std::endl;
                        }
                        timeoutTimer.cancel();
                    }
                });
        }

        void readRestOfHttpHeader() {
            boost::asio::async_read_until(*socket, buffer,
                UntilFunction(MatchStringOrSize("\r\n\r\n", MAX_HEADER_SIZE - bytesRead)),
                [this, capture = shared_from_this()] (const boost::system::error_code &e, size_t /*size*/) {
                    timeoutTimer.cancel();

                    if (e) {
                        if (VERBOSE_LOGGING) {
                            std::cerr << "HTTP client error: " << e.message() << std::endl;
                        }
                        return;
                    }

                    server.udpServer.addUdpToHttpReceiver(socket, udpEndpoint);
                });
        }

        std::shared_ptr<tcp::socket> socket;
        boost::asio::streambuf buffer{MAX_HEADER_SIZE};
        size_t bytesRead = 0;
        boost::asio::system_timer timeoutTimer;
        Server &server;
        boost::asio::ip::udp::endpoint udpEndpoint;
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
};

}
