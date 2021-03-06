#pragma once

#include "simple_http_server.h"
#include "udp_to_tcp_proxy_server.h"
#include "ssl_stream_factory.h"

namespace UdpProxy {

template <typename Allocator, bool SendHttpResponses>
class BasicUdpToHttpProxyServer {
public:
    BasicUdpToHttpProxyServer(boost::asio::io_service &ioService, const tcp::endpoint &endpoint)
            : udpServer(ioService), httpServer(ioService, endpoint, *this, sslStreamFactory) {
        sslStreamFactory.enableSsl(false);
        udpServer.setStartUdpInputHandler(std::bind(&BasicUdpToHttpProxyServer::startUdpInputHandler, this, std::placeholders::_1, std::placeholders::_2));
        udpServer.setReadUdpInputHandler(std::bind(&BasicUdpToHttpProxyServer::readUdpInputHandler, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        udpServer.setStartClientHandler(std::bind(&BasicUdpToHttpProxyServer::startClientHandler, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
        udpServer.setWriteClientHandler(std::bind(&BasicUdpToHttpProxyServer::writeClientHandler, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
    }

    void runAsync() {
        udpServer.detectDisconnectedClientsAsync();
        httpServer.runAsync();
    }

    void enableSsl(bool enable) { sslStreamFactory.enableSsl(enable); }
    ssl::context& getSslContext() { return sslStreamFactory.getContext(); }

    void setMaxHttpHeaderSize(size_t value) noexcept { httpServer.setMaxHttpHeaderSize(value); }
    size_t getMaxHttpHeaderSize() const noexcept { return httpServer.getMaxHttpHeaderSize(); }
    void setHttpConnectionTimeout(boost::asio::system_timer::duration value) noexcept { httpServer.setHttpConnectionTimeout(value); }
    boost::asio::system_timer::duration getHttpConnectionTimeout() const noexcept { return httpServer.getHttpConnectionTimeout(); }
    void setMaxUdpDataSize(size_t value) noexcept { udpServer.setMaxUdpDataSize(value); }
    size_t getMaxUdpDataSize() const noexcept { return udpServer.getMaxUdpDataSize(); }
    void setMaxOutputQueueLength(size_t value) noexcept { udpServer.setMaxOutputQueueLength(value); }
    size_t getMaxOutputQueueLength() const noexcept { return udpServer.getMaxOutputQueueLength(); }
    void setMaxHttpClients(size_t value) noexcept { httpServer.setMaxHttpClients(value); }
    size_t getMaxHttpClients() const noexcept { return httpServer.getMaxHttpClients(); }
    void setOutputQueueOverflowAlgorithm(OutputQueueOverflowAlgorithm value) noexcept { udpServer.setOutputQueueOverflowAlgorithm(value); }
    OutputQueueOverflowAlgorithm getOutputQueueOverflowAlgorithm() const noexcept { return udpServer.getOutputQueueOverflowAlgorithm(); };
    void setVerboseLogging(bool value) noexcept {
        verboseLogging = value;
        udpServer.setVerboseLogging(value);
        httpServer.setVerboseLogging(value);
    }
    bool getVerboseLogging() const noexcept { return verboseLogging; }
    void setEnableStatus(bool value) noexcept { enableStatus = value; }
    bool getEnableStatus() const noexcept { return enableStatus; }
    void setRenewMulticastSubscriptionInterval(boost::asio::system_timer::duration value) noexcept { udpServer.setRenewMulticastSubscriptionInterval(value); }
    boost::asio::system_timer::duration getRenewMulticastSubscriptionInterval() const noexcept { return udpServer.getRenewMulticastSubscriptionInterval(); }
    void setMulticastInterfaceAddress(boost::asio::ip::address value) noexcept { udpServer.setMulticastInterfaceAddress(value); }
    boost::asio::ip::address getMulticastInterfaceAddress() const noexcept { return udpServer.getMulticastInterfaceAddress(); }

private:
    typedef UdpProxy::SimpleHttpServer<BasicUdpToHttpProxyServer&, SslStreamFactory&, Allocator> HttpServer;
    typedef UdpProxy::HttpRequest<BasicUdpToHttpProxyServer&, SslStreamFactory&, Allocator> HttpRequest;
    friend HttpServer;

    struct UdpInputCustomData {
        std::chrono::system_clock::time_point bitrateCalculationStart;
        size_t bytesIn = 0;
        double inBitrateKbit = 0;
        size_t clientsCount = 0;
    };

    struct ClientCustomData {
        std::shared_ptr<HttpRequest> request;
        std::chrono::system_clock::time_point bitrateCalculationStart = std::chrono::system_clock::time_point::min();
        size_t bytesOut = 0;
        double outBitrateKbit = 0;
    };

    void writeJsonStatus(const std::shared_ptr<HttpRequest>& request) {
        auto stream = request->getStream().lock();

        if (verboseLogging) {
            std::cerr << "status HTTP client: " << stream->lowest_layer().remote_endpoint() << std::endl;
        }

        static constexpr std::experimental::string_view header =
            "HTTP/1.1 200 OK\r\n"
            "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
            "Content-Type: application/json\r\n"
            "Connection: close\r\n"
            "\r\n"sv;

        // TODO: optimize JSON output
        std::ostringstream json;

        json << R"({"inputs":[)";
        bool first = true;
        auto udpInputs = udpServer.getUdpInputs();
        for (const auto& udpInput : udpInputs) {
            if (first) {
                first = false;
                json << '{';
            } else {
                json << ",{";
            }

            json << R"("endpoint":")" << udpInput.endpoint << R"(",)";
            json << R"("clients_count":)" << udpInput.clients.size() << ',';
            json << R"("bitrate":)" << udpInput.customData.inBitrateKbit << '}';
        }

        json << R"(],"clients":[)";

        first = true;
        for (const auto& udpInput : udpInputs) {
            for (const auto& client : udpInput.clients) {
                if (first) {
                    first = false;
                    json << '{';
                } else {
                    json << ",{";
                }

                json << R"("remote_endpoint":")" << client.endpoint << R"(",)";
                json << R"("udp_endpoint":")" << udpInput.endpoint << R"(",)";
                json << R"("output_queue_length":)" << client.outputQueueLength << ',';
                json << R"("bitrate":)" << client.customData.outBitrateKbit << '}';
            }
        }
        json << "]}";

        // TODO: optimize to use direct buffer access without copying
        auto response = std::make_shared<std::string>(json.str());

        boost::asio::async_write(*stream, boost::asio::buffer(header.cbegin(), header.length()),
            [verboseLogging = verboseLogging, request = request, response = response] (const boost::system::error_code &e, std::size_t /*bytesSent*/) {
                if (request->getStream().expired()) {
                    return;
                }

                if (e) {
                    if (verboseLogging) {
                        std::cerr << "status HTTP header write error: " << e.message() << std::endl;
                    }
                    return;
                }

                boost::asio::async_write(*request->getStream().lock(), boost::asio::buffer(response->c_str(), response->length()),
                    [verboseLogging = verboseLogging, request = request, response = response] (const boost::system::error_code &e, std::size_t /*bytesSent*/) {
                        if (e && verboseLogging) {
                            std::cerr << "status HTTP body write error: " << e.message() << std::endl;
                        }
                    });
            });
    }

    void writeNotFoundResponse(const std::shared_ptr<HttpRequest>& request) {
        if (verboseLogging) {
            std::cerr << "wrong URI: " << request->getUri() << std::endl;
        }

        writeResponse(request,
            "HTTP/1.1 404 Not Found\r\n"
            "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: close\r\n"
            "\r\n"
            "404 Not Found"sv);
    }

    void writeResponse(const std::shared_ptr<HttpRequest>& request, std::experimental::string_view response) {
        if (SendHttpResponses) {
            boost::asio::async_write(*request->getStream().lock(), boost::asio::buffer(response.cbegin(), response.length()),
                [request = request] (const boost::system::error_code &/*e*/, std::size_t /*bytesSent*/) {});
        }
    }

    void handleRequestError(std::shared_ptr<HttpRequest> request, RequestError error) {
        static std::experimental::string_view response;

        switch (error) {
        case RequestError::ClientsLimitReached:
            response =
                "HTTP/1.1 429 Too Many Requests\r\n"
                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n"
                "\r\n"
                "Maximum number of clients reached"sv;
            break;

        case RequestError::HttpHeaderTooLarge:
            response =
                "HTTP/1.1 431 Request Header Fields Too Large\r\n"
                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n"
                "\r\n"
                "Total size of HTTP request header is too large"sv;
            break;

        case RequestError::RequestTimeout:
            response =
                "HTTP/1.1 408 Request Timeout\r\n"
                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                "Connection: close\r\n"
                "\r\n"sv;
            break;

        case RequestError::BadHttpRequest:
            response =
                "HTTP/1.1 400 Bad Request\r\n"
                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                "Connection: close\r\n"
                "\r\n"sv;
            break;
        }

        writeResponse(request, response);
    }

    void handleRequest(std::shared_ptr<HttpRequest> request) {
        auto uri = request->getUri();
        auto stream = request->getStream().lock();

        if (verboseLogging) {
            std::cerr << "request: " << request->getMethod() << ' ' << uri << ' ' << request->getProtocolVersion() << std::endl;
        }

        if (request->getMethod() != "GET") {
            writeResponse(request,
                "HTTP/1.1 501 Not Implemented\r\n"
                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                "Connection: close\r\n"
                "\r\n"sv);
            return;
        } else if (request->getProtocolVersion() != "HTTP/1.1") {
            writeResponse(request,
                "HTTP/1.1 505 HTTP Version Not Supported\r\n"
                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n"
                "\r\n"
                "Only HTTP/1.1 is supported"sv);
            return;
        }

        if (uri == "/status") {
            if (enableStatus) {
                writeJsonStatus(request);
            } else {
                writeNotFoundResponse(request);
            }
            return;
        }

        if (uri.length() < "/udp/1.2.3.4:5"sv.length()) {
            writeNotFoundResponse(request);
            return;
        }

        static constexpr auto UDP_URI_PREFIX = "/udp/"sv;

        if (uri.substr(0, UDP_URI_PREFIX.length()) != UDP_URI_PREFIX) {
            writeNotFoundResponse(request);
            return;
        }

        size_t portBegin = uri.find(':', UDP_URI_PREFIX.length());
        if ((portBegin == std::experimental::string_view::npos)
                || (portBegin > "/udp/123.123.123.123"sv.length())
                || (portBegin < "/udp/1.2.3.4"sv.length())) {
            writeNotFoundResponse(request);
            return;
        }

        portBegin += 1;

        size_t portEnd = uri.find('?', UDP_URI_PREFIX.length());
        if (portEnd == std::experimental::string_view::npos) {
            portEnd = uri.length();
        }

        if ((portEnd - portBegin > 5) || (portEnd - portBegin < 1)) {
            writeNotFoundResponse(request);
            return;
        }

        boost::asio::ip::address address;
        unsigned long port;

        try {
            // TODO: use std::from_chars to avoid std::string creation and memory copying
            port = std::stoul(std::string(uri.cbegin() + portBegin, portEnd - portBegin));
            // TODO: avoid std::string creation and memory copying
            // possible only when ip::address::from_string supports string_view
            address = boost::asio::ip::address::from_string(std::string(uri.cbegin() + UDP_URI_PREFIX.length(), portBegin - 1 - UDP_URI_PREFIX.length()));
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
            udpServer.addClient(stream, udpEndpoint, ClientCustomData{request});
        } catch (const boost::system::system_error &e) {
            std::cerr << "UDP socket error for " << udpEndpoint << ": " << e.what() << std::endl;
            writeResponse(request,
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n"
                "\r\n"
                "UDP socket error"sv);
        }

        request->cancelTimeout();

        static constexpr std::experimental::string_view response =
            "HTTP/1.1 200 OK\r\n"
            "Server: " UDPPROXY_SERVER_NAME_DEFINE "\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Connection: close\r\n"
            "\r\n"sv;

        boost::asio::async_write(*stream, boost::asio::buffer(response.cbegin(), response.length()),
            [this, request = request, udpEndpoint, clientEndpoint = stream->lowest_layer().remote_endpoint()] (const boost::system::error_code &e, std::size_t /*bytesSent*/) {
                if (request->getStream().expired()) {
                    return;
                }

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

    void startClientHandler(const tcp::endpoint &/*clientEndpoint*/, const udp::endpoint &/*udpEndpoint*/, ClientCustomData &clientData, UdpInputCustomData &/*udpInputData*/) const noexcept {
        clientData.bitrateCalculationStart = std::chrono::system_clock::now();
    }

    void writeClientHandler(const tcp::endpoint &/*clientEndpoint*/, const udp::endpoint &/*udpEndpoint*/, size_t bytesWritten, ClientCustomData &clientData, UdpInputCustomData &/*udpInputData*/) const noexcept {
        static constexpr std::chrono::system_clock::duration BITRATE_PERIOD = 5s;

        auto now = std::chrono::system_clock::now();
        auto duration = now - clientData.bitrateCalculationStart;

        if (duration >= BITRATE_PERIOD) {
            clientData.outBitrateKbit = 8. * clientData.bytesOut / std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

            clientData.bitrateCalculationStart = now;
            clientData.bytesOut = bytesWritten;
        } else {
            clientData.bytesOut += bytesWritten;
        }
    }

    SslStreamFactory sslStreamFactory;
    UdpToTcpProxyServer<Allocator, UdpInputCustomData, ClientCustomData, typename HttpServer::StreamType> udpServer;
    HttpServer httpServer;

    bool verboseLogging = true;
    bool enableStatus = false;
};

typedef BasicUdpToHttpProxyServer<std::allocator<uint8_t>, true> UdpToHttpProxyServer;

}
