#include <udp_proxy/udp_proxy.h>

#include <boost/program_options.hpp>

int main(int argc, const char * const argv[]) {
    //boost::asio::ip::address address;
    uint16_t port;
    size_t maxClients;
    size_t maxUdpDataSize;
    size_t maxWriteQueueLength;
    bool verboseLogging;

    namespace po = boost::program_options;
    po::options_description description("Options");
    description.add_options()
        ("help,h", "Help message")
        ("port,p", po::value<uint16_t>(&port)->default_value(5000) , "Port to listen on")
        /*("listen,l", po::value<std::string>()->default_value("255.255.255.255")->notifier([&address] (const std::string &addressString) {
            try {
                address = boost::asio::ip::address::from_string(addressString);
            } catch (const boost::system::system_error &e) {
                throw po::error("the argument ('" + addressString + "') for option '--listen' is invalid");
            }
        }), "Address to listen on")*/
        ("clients,c", po::value<size_t>(&maxClients)->default_value(0), "Maximum number of clients to accept (0 = unlimited)")
        ("buffer,B", po::value<size_t>(&maxUdpDataSize)->default_value(4 * 1024), "Maximum UDP packet data size")
        ("writeq,R", po::value<size_t>(&maxWriteQueueLength)->default_value(1024), "Maximum write queue length per client (0 = unlimited)")
        ("verbose,v", "Enable verbose output");

    po::variables_map variablesMap;
    try {
        po::store(po::parse_command_line(argc, argv, description), variablesMap);
        po::notify(variablesMap);
    } catch (const po::error &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    if (variablesMap.count("help")) {
        std::cout << description << std::endl;
        return 0;
    }

    verboseLogging = variablesMap.count("verbose");

    try {
        boost::asio::io_service ioService;
        UdpProxy::Server server(ioService, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4()/*address*/, port));

        server.setMaxHttpClients(maxClients);
        server.setMaxUdpDataSize(maxUdpDataSize);
        server.setMaxWriteQueueLength(maxWriteQueueLength);
        server.setVerboseLogging(verboseLogging);

        std::cout << "Running on port " << port << std::endl;
        ioService.run();
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    return 0;
}
