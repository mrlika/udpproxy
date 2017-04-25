#include <udp_proxy/udp_proxy.h>

int main() {
    try {
        boost::asio::io_service ioService;
        UdpProxy::HttpServer server(ioService);
        ioService.run();
    } catch (std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    return 0;
}
