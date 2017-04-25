#pragma once

#include <boost/asio.hpp>

#include <iostream>
#include <memory>

namespace UdpProxy {

using boost::asio::ip::tcp;

class HttpServer {
private:
    tcp::acceptor acceptor;

public:
    HttpServer(boost::asio::io_service &ioService)
            : acceptor(ioService, tcp::endpoint(tcp::v4(), 10013)) {
        startAccept();
    }

    void startAccept() {
        auto socket = std::make_shared<tcp::socket>(acceptor.get_io_service());

        acceptor.async_accept(*socket, [this, socket] (const boost::system::error_code &e) {
            if (!e) {
                handleAccept(socket);
            } else {
                std::cout << e.message() << std::endl;
            }

            startAccept();
        });
    }

    void handleAccept(std::shared_ptr<tcp::socket> socket) {
        time_t now = time(0);

        boost::asio::async_write(*socket, boost::asio::buffer(std::string(ctime(&now))), [this] (const boost::system::error_code &e, size_t bytesTransferred) {
            if (!e) {
                handleWrite(bytesTransferred);
            } else {
                std::cout << e.message() << std::endl;
            }
        });
    }

    void handleWrite(size_t bytesTransferred) {
        std::cout << "bytes transferred: " << bytesTransferred << std::endl;
    }
};

}
