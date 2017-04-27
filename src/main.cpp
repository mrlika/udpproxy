#include <udp_proxy/udp_proxy.h>

int main(int argc, const char * const argv[]) {
    unsigned short port = 6000;
    if (argc >= 2) {
        try {
            port = std::stoul(argv[1]);
        } catch(...) {
        }
    }

    try {
        boost::asio::io_service ioService;
        UdpProxy::Server server(ioService, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port));
        std::cout << "Running on port " << port << std::endl;
        ioService.run();
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    return 0;
}
