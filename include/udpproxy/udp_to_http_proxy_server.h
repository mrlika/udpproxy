#pragma once

#include "simple_http_server.h"
#include "udp_to_tcp_proxy_server.h"

#include <regex>

namespace UdpProxy {

template <typename Allocator, bool SendHttpResponses>
class BasicUdpToHttpProxyServer {
public:
    BasicUdpToHttpProxyServer(boost::asio::io_service &ioService, const tcp::endpoint &endpoint)
            : udpServer(ioService), httpServer(ioService, endpoint) {
        httpServer.setRequestHandler(std::bind(&BasicUdpToHttpProxyServer::handleRequest, this, std::placeholders::_1));
        udpServer.setStartUdpInputHandler(std::bind(&BasicUdpToHttpProxyServer::startUdpInputHandler, this, std::placeholders::_1, std::placeholders::_2));
        udpServer.setReadUdpInputHandler(std::bind(&BasicUdpToHttpProxyServer::readUdpInputHandler, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    }

    void runAsync() {
        udpServer.detectDisconnectedClientsAsync();
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
    void writeJsonStatus() {
        /*if (verboseLogging) {
            std::cerr << "status HTTP client: " << socket->remote_endpoint() << std::endl;
        }

        // TODO: optimize JSON output
        std::ostringstream out;

        out << R"({"inputs":[)";
        bool first = true;
        for (const auto& udpInputItem : server.udpProxy.udpInputs) {
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
        for (const auto& udpInput : server.udpProxy.udpInputs) {
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
            [this, reference = std::weak_ptr<HttpClient>(this->shared_from_this()), response = response] (const boost::system::error_code &e, std::size_t bytesSent) {
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
                    [this, reference = std::weak_ptr<HttpClient>(this->shared_from_this()), response = response] (const boost::system::error_code &e, std::size_t bytesSent) {
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

    struct UdpInputCustomData {
        std::chrono::system_clock::time_point bitrateCalculationStart;
        size_t bytesIn = 0;
        double inBitrateKbit = 0;
        size_t clientsCount = 0;
    };

    struct ClientCustomData {
        std::shared_ptr<typename SimpleHttpServer<Allocator, SendHttpResponses>::HttpRequest> request;
        std::chrono::system_clock::time_point bitrateCalculationStart = std::chrono::system_clock::time_point::min();
        size_t bytesOut = 0;
        double outBitrateKbit = 0;
    };

    void writeNotFoundResponse(const std::shared_ptr<typename SimpleHttpServer<Allocator, SendHttpResponses>::HttpRequest>& request) {
        if (verboseLogging) {
            std::cerr << "wrong URI: " << request->getUri() << std::endl;
        }

        static constexpr auto response =
            "HTTP/1.1 404 Not Found\r\n"
            "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: close\r\n"
            "\r\n"
            "404 Not Found"sv;

        boost::asio::async_write(*request->getSocket().lock(), boost::asio::buffer(response.cbegin(), response.length()),
            [request = request] (const boost::system::error_code &/*e*/, std::size_t /*bytesSent*/) {});
    }

    void handleRequest(std::shared_ptr<typename SimpleHttpServer<Allocator, SendHttpResponses>::HttpRequest> request) {
        auto uri = request->getUri();
        auto socket = request->getSocket().lock();

        if (verboseLogging) {
            std::cerr << "request: " << uri << std::endl;
        }

        if (uri == "/status") {
            static constexpr auto response =
                "HTTP/1.1 200 OK\r\n"
                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n"
                "\r\nStatus"sv;

            boost::asio::async_write(*socket, boost::asio::buffer(response.cbegin(), response.length()),
                [request = request] (const boost::system::error_code &/*e*/, std::size_t /*bytesSent*/) {});
            return;
        }

        // TODO: replace regex with parsing algorithm for better performance and to avoid memory allocations
        static const std::regex uriRegex("/udp/(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}):(\\d{1,5})(?:\\?.*)?", std::regex_constants::optimize);
        std::cmatch match;
        std::regex_match(uri.begin(), uri.end(), match, uriRegex);

        if (match.empty()) {
            writeNotFoundResponse(request);
            return;
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
            writeNotFoundResponse(request);
            return;
        }

        if ((port == 0) || (port > std::numeric_limits<uint16_t>::max())) {
            writeNotFoundResponse(request);
            return;
        }

        udp::endpoint udpEndpoint = {address, static_cast<unsigned short>(port)};

        try {
            udpServer.addClient(socket, udpEndpoint, ClientCustomData{request});
        } catch (const boost::system::system_error &e) {
            std::cerr << "UDP socket error for " << udpEndpoint << ": " << e.what() << std::endl;
            static constexpr auto response =
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n"
                "\r\n"
                "UDP socket error"sv;
            boost::asio::async_write(*socket, boost::asio::buffer(response.cbegin(), response.length()),
                [request = request] (const boost::system::error_code &/*e*/, std::size_t /*bytesSent*/) {});
        }

        request->cancelTimeout();

        static constexpr std::experimental::string_view response =
            "HTTP/1.1 200 OK\r\n"
            "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Connection: close\r\n"
            "\r\n"sv;

        boost::asio::async_write(*socket, boost::asio::buffer(response.cbegin(), response.length()),
            [this, request = request, udpEndpoint] (const boost::system::error_code &e, std::size_t /*bytesSent*/) {
                if (request->getSocket().expired()) {
                    return;
                }

                auto clientEndpoint = request->getSocket().lock()->remote_endpoint();

                if (e) {
                    if (verboseLogging) {
                        std::cerr << "HTTP header write error for " << clientEndpoint << ": " << e.message() << std::endl;
                    }
                    return;
                }

                udpServer.startClient(clientEndpoint, udpEndpoint);
            });
    }

    void startUdpInputHandler(const udp::endpoint &/*udpEndpoint*/, UdpInputCustomData& udpInputData) const noexcept {
        udpInputData.bitrateCalculationStart = std::chrono::system_clock::now();
    }

    void readUdpInputHandler(const udp::endpoint &/*udpEndpoint*/, size_t bytesRead, UdpInputCustomData& udpInputData) const noexcept {
        static constexpr std::chrono::system_clock::duration BITRATE_PERIOD = 5s;

        auto now = std::chrono::system_clock::now();
        auto duration = now - udpInputData.bitrateCalculationStart;

        if (duration >= BITRATE_PERIOD) {
            udpInputData.inBitrateKbit = 8. * udpInputData.bytesIn / std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

            udpInputData.bitrateCalculationStart = now;
            udpInputData.bytesIn = bytesRead;
        } else {
            udpInputData.bytesIn += bytesRead;
        }
    }

    UdpToTcpProxyServer<Allocator, UdpInputCustomData, ClientCustomData> udpServer;
    SimpleHttpServer<Allocator, SendHttpResponses> httpServer;

    bool verboseLogging = true;
    bool enableStatus = false;
};

typedef BasicUdpToHttpProxyServer<std::allocator<uint8_t>, true> UdpToHttpProxyServer;

}
