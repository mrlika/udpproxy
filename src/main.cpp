#include <udpproxy/udp_to_http_proxy_server.h>

#include <boost/program_options.hpp>

namespace po = boost::program_options;

namespace UdpProxy {

std::istream& operator>>(std::istream &in, OutputQueueOverflowAlgorithm &algorithm) {
    std::string token;
    in >> token;

    if (token == "clearq") {
        algorithm = OutputQueueOverflowAlgorithm::ClearQueue;
    } else if (token == "drop") {
        algorithm = OutputQueueOverflowAlgorithm::DropData;
    } else {
        throw po::validation_error(po::validation_error::invalid_option_value, "wqoverflow", token);
    }

    return in;
}

std::ostream& operator<<(std::ostream &out, OutputQueueOverflowAlgorithm algorithm) {
    switch (algorithm) {
    case OutputQueueOverflowAlgorithm::ClearQueue:
        out << "clearq";
        break;

    case OutputQueueOverflowAlgorithm::DropData:
        out << "drop";
        break;
    }

    return out;
}

}

int main(int argc, const char * const argv[]) {
    boost::asio::ip::address address;
    uint16_t port;
    size_t maxClients;
    size_t maxUdpDataSize;
    size_t maxOutputQueueLength;
    UdpProxy::OutputQueueOverflowAlgorithm overflowAlgorithm;
    bool verboseLogging;
    bool enableStatus;
    unsigned renewMulitcastSubscriptionInterval;
    boost::asio::ip::address multicastInterfaceAddress;
    unsigned httpConnectionTimeout;
    size_t maxHttpHeaderSize;
    std::string keyFile;
    std::string certificateChainFile;
    std::string keyFilePassword;

    std::cout << UdpProxy::SERVER_NAME << std::endl;

    po::options_description description("Options");
    description.add_options()
        ("help,h", "Print help message")
        ("port,p", po::value<uint16_t>(&port)->default_value(5000) , "Port to listen on")
        ("listen,a", po::value<std::string>()->default_value("0.0.0.0")->notifier([&address] (const std::string &token) {
            try {
                address = boost::asio::ip::address::from_string(token);
            } catch (const boost::system::system_error&) {
                throw po::validation_error(po::validation_error::invalid_option_value, "listen", token);
            }
        }), "Address to listen on")
        ("clients,c", po::value<size_t>(&maxClients)->default_value(0), "Maximum number of clients to accept (0 = unlimited)")
        ("buffer,B", po::value<size_t>(&maxUdpDataSize)->default_value(4 * 1024), "Maximum UDP packet data size in bytes")
        ("outputq,R", po::value<size_t>(&maxOutputQueueLength)->default_value(1024), "Maximum output queue length per client (0 = unlimited)")
        ("oqoverflow,o", po::value<UdpProxy::OutputQueueOverflowAlgorithm>(&overflowAlgorithm)->default_value(UdpProxy::OutputQueueOverflowAlgorithm::ClearQueue),
            "Output queue overflow algorithm: 'clearq' (clear queue) or 'drop' (drop current input data)")
        ("verbose,v", "Enable verbose output")
        ("status,S", "Enable /status URL")
        ("renew,M", po::value<unsigned>(&renewMulitcastSubscriptionInterval)->default_value(0), "Renew multicast subscription interval in seconds (0 = disable)")
        ("multicastif,m", po::value<std::string>()->default_value("0.0.0.0")->notifier([&multicastInterfaceAddress] (const std::string &token) {
            try {
                multicastInterfaceAddress = boost::asio::ip::address::from_string(token);
            } catch (const boost::system::system_error&) {
                throw po::validation_error(po::validation_error::invalid_option_value, "multicastif", token);
            }

            if (!multicastInterfaceAddress.is_v4()) {
                throw po::validation_error(po::validation_error::invalid_option_value, "multicastif", token);
            }
        }), "Multicast interface IP address")
        ("httptimeout,T", po::value<unsigned>(&httpConnectionTimeout)->default_value(1), "Timeout for HTTP connections in seconds (0 = disable)")
        ("httpheader,H", po::value<size_t>(&maxHttpHeaderSize)->default_value(4 * 1024), "Maximum input HTTP header size in bytes")
        ("key", po::value<std::string>(&keyFile)->default_value(""), "Private key file in PEM format for SSL\\TLS")
        ("keypass", po::value<std::string>(&keyFilePassword)->default_value(""), "Private key file password")
        ("cert", po::value<std::string>(&certificateChainFile)->default_value(""), "Certificate or full chain file in PEM format for SSL\\TLS");

    po::variables_map variablesMap;
    try {
        po::store(po::parse_command_line(argc, argv, description), variablesMap);
        po::notify(variablesMap);
    } catch (const po::error &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    if (variablesMap.count("help") != 0) {
        std::cout << description << std::endl;
        return 0;
    }

    verboseLogging = (variablesMap.count("verbose") != 0);
    enableStatus = (variablesMap.count("status") != 0);

    try {
        boost::asio::io_service ioService;
        UdpProxy::UdpToHttpProxyServer server(ioService, boost::asio::ip::tcp::endpoint(address, port));

        server.setMaxHttpClients(maxClients);
        server.setMaxUdpDataSize(maxUdpDataSize);
        server.setMaxOutputQueueLength(maxOutputQueueLength);
        server.setOutputQueueOverflowAlgorithm(overflowAlgorithm);
        server.setVerboseLogging(verboseLogging);
        server.setEnableStatus(enableStatus);
        server.setRenewMulticastSubscriptionInterval(std::chrono::seconds(renewMulitcastSubscriptionInterval));
        server.setMulticastInterfaceAddress(multicastInterfaceAddress);
        server.setHttpConnectionTimeout(std::chrono::seconds(httpConnectionTimeout));
        server.setMaxHttpHeaderSize(maxHttpHeaderSize);

        if (!keyFile.empty() && !certificateChainFile.empty()) {
            server.enableSsl(true);
            boost::asio::ssl::context& context = server.getSslContext();
            context.set_options(
                boost::asio::ssl::context::default_workarounds
                | boost::asio::ssl::context::no_sslv2
                | boost::asio::ssl::context::single_dh_use);
            context.use_private_key_file(keyFile, boost::asio::ssl::context::pem);
            context.use_certificate_chain_file(certificateChainFile);
            if (!keyFilePassword.empty()) {
                context.set_password_callback([keyFilePassword] (size_t maxLength, boost::asio::ssl::context_base::password_purpose /*purpose*/) {
                    return keyFilePassword.length() < maxLength ? keyFilePassword : "";
                });
            }
        }

        server.runAsync();

        std::cout << "Running on port " << port << std::endl;

        ioService.run();
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    return 0;
}
