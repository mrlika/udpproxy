#include <udp_proxy/udp_proxy.h>

int main() {
    try {
        boost::asio::io_service ioService;
        UdpProxy::Server server(ioService, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 10013));
        ioService.run();
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    return 0;
}
