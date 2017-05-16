#pragma once

#include "simple_http_server.h"
#include "udp_proxy_service.h"

#include <regex>

namespace UdpProxy {

template <typename Allocator, bool SendHttpResponses>
class BasicUdpToHttpProxyServer {
public:
    BasicUdpToHttpProxyServer(boost::asio::io_service &ioService, const tcp::endpoint &endpoint)
            : udpProxyService(ioService), httpServer(ioService, endpoint) {
        httpServer.setRequestHandler(std::bind(&BasicUdpToHttpProxyServer::handleRequest, this, std::placeholders::_1));
    }

    void runAsync() {
        udpProxyService.runAsync();
        httpServer.runAsync();
    }

    void setMaxHttpHeaderSize(size_t value) { httpServer.setMaxHttpHeaderSize(value); }
    size_t getMaxHttpHeaderSize() const noexcept { return httpServer.getMaxHttpHeaderSize(); }
    void setHttpConnectionTimeout(boost::asio::system_timer::duration value) { httpServer.setHttpConnectionTimeout(value); }
    boost::asio::system_timer::duration getHttpConnectionTimeout() const noexcept { return httpServer.getHttpConnectionTimeout(); }
    void setMaxUdpDataSize(size_t value) { udpProxyService.setMaxUdpDataSize(value); }
    size_t getMaxUdpDataSize() const noexcept { return udpProxyService.getMaxUdpDataSize(); }
    void setMaxOutputQueueLength(size_t value) { udpProxyService.setMaxOutputQueueLength(value); }
    size_t getMaxOutputQueueLength() const noexcept { return udpProxyService.getMaxOutputQueueLength(); }
    void setMaxHttpClients(size_t value) { httpServer.setMaxHttpClients(value); }
    size_t getMaxHttpClients() const noexcept { return httpServer.getMaxHttpClients(); }
    void setOutputQueueOverflowAlgorithm(OutputQueueOverflowAlgorithm value) { udpProxyService.setOutputQueueOverflowAlgorithm(value); }
    OutputQueueOverflowAlgorithm getOutputQueueOverflowAlgorithm() const noexcept { return udpProxyService.getOutputQueueOverflowAlgorithm(); };
    void setVerboseLogging(bool value) {
        verboseLogging = value;
        udpProxyService.setVerboseLogging(value);
        httpServer.setVerboseLogging(value);
    }
    bool getVerboseLogging() const noexcept { return verboseLogging; }
    void setEnableStatus(bool value) { enableStatus = value; }
    bool getEnableStatus() const noexcept { return enableStatus; }
    void setRenewMulticastSubscriptionInterval(boost::asio::system_timer::duration value) { udpProxyService.setRenewMulticastSubscriptionInterval(value); }
    boost::asio::system_timer::duration getRenewMulticastSubscriptionInterval() const noexcept { return udpProxyService.getRenewMulticastSubscriptionInterval(); }
    void setMulticastInterfaceAddress(boost::asio::ip::address value) { udpProxyService.setMulticastInterfaceAddress(value); }
    boost::asio::ip::address getMulticastInterfaceAddress() const noexcept { return udpProxyService.getMulticastInterfaceAddress(); }

private:
    void addUdpClient() {
        /*try {
            udpProxy.addUdpToHttpClient(socket, udpEndpoint);
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

    void handleRequest(std::shared_ptr<typename SimpleHttpServer<SendHttpResponses>::HttpRequest> request) {
        auto uri = request->getUri();

        if (uri == "/status") {
            static constexpr std::experimental::string_view response =
                "HTTP/1.1 200 OK\r\n"
                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n"
                "\r\nStatus"sv;

            boost::asio::async_write(request->getSocket(), boost::asio::buffer(response.cbegin(), response.length()),
                [request = request] (const boost::system::error_code &/*e*/, std::size_t /*bytesSent*/) {
                    if (request->isExpired()) {
                        return;
                    }
                });

            return;
        }

        // TODO: replace regex with parsing algorithm for better performance and to avoid memory allocations
        static const std::regex uriRegex("/udp/(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}):(\\d{1,5})(?:\\?.*)?", std::regex_constants::optimize);
        std::cmatch match;
        std::regex_match(uri.begin(), uri.end(), match, uriRegex);
    }

    UdpProxyService<Allocator> udpProxyService;
    SimpleHttpServer<SendHttpResponses> httpServer;

    bool verboseLogging = true;
    bool enableStatus = false;
};

typedef BasicUdpToHttpProxyServer<std::allocator<uint8_t>, true> UdpToHttpProxyServer;

}
