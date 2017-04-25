#pragma once

#include <boost/asio.hpp>

#include <iostream>
#include <memory>
#include <array>
#include <experimental/string_view>

namespace UdpProxy {

using boost::asio::ip::tcp;

class Server {
public:
    Server(boost::asio::io_service &ioService)
            : acceptor(ioService, tcp::endpoint(tcp::v4(), 10013)) {
        startAccept();
    }

private:
    constexpr static size_t MAX_HEADER_SIZE = 4 * 1024;

    tcp::acceptor acceptor;

    typedef boost::asio::buffers_iterator<boost::asio::streambuf::const_buffers_type> UntilIterator;
    typedef std::function<std::pair<UntilIterator, bool>(UntilIterator begin, UntilIterator end) noexcept> UntilFunction;

    void startAccept() {
        auto socket = std::make_shared<tcp::socket>(acceptor.get_io_service());

        acceptor.async_accept(*socket, [this, socket] (const boost::system::error_code &e) mutable {
            if (!e) {
                validateHttpMethod(std::move(socket));
            } else {
                std::cout << e.message() << std::endl;
            }

            startAccept();
        });
    }

    void validateHttpMethod(std::shared_ptr<tcp::socket> socket) {
        // TODO: start timeout timer

        constexpr static std::experimental::string_view REQUEST_METHOD_STRING("GET ", 4);

        auto buffer = std::make_shared<boost::asio::streambuf>();

        boost::asio::async_read_until(*socket, *buffer,
            UntilFunction(MatchStringOrSize(REQUEST_METHOD_STRING, REQUEST_METHOD_STRING.size())),
            [this, socket, buffer] (const boost::system::error_code& e, size_t size) mutable {
                if (!e) {
                    std::experimental::string_view method(boost::asio::buffer_cast<const char*>(buffer->data()), REQUEST_METHOD_STRING.size());
                    if (method == REQUEST_METHOD_STRING) {
                        buffer->consume(size);
                        readHttpRequestUri(socket, buffer, size);
                    } else {
                        std::cout << "error: method not supported" << std::endl;
                    }
                } else {
                    std::cout << "error: " << e.message() << std::endl;
                }
            });
    }

    void readHttpRequestUri(std::shared_ptr<tcp::socket> &socket, std::shared_ptr<boost::asio::streambuf> &buffer, size_t bytesRead) {
        constexpr static std::experimental::string_view REQUEST_LINE_ENDING(" HTTP/1.1\r\n", 11);

        boost::asio::async_read_until(*socket, *buffer,
            UntilFunction(MatchStringOrSize(REQUEST_LINE_ENDING, MAX_HEADER_SIZE - bytesRead)),
            [this, socket, buffer, bytesRead] (const boost::system::error_code& e, size_t size) mutable {
                if (!e) {
                    std::experimental::string_view uri(boost::asio::buffer_cast<const char*>(buffer->data()), size - REQUEST_LINE_ENDING.size());
                    std::cout << "URI: '" << uri << '\'' << std::endl;
                    buffer->consume(size - 2); // Do not consume CRLF
                    readRestOfHttpHeader(socket, buffer, bytesRead + size - 2);
                } else {
                    std::cout << "error: " << e.message() << std::endl;
                }
            });
    }

    void readRestOfHttpHeader(std::shared_ptr<tcp::socket> &socket, std::shared_ptr<boost::asio::streambuf> &buffer, size_t bytesRead) {
        boost::asio::async_read_until(*socket, *buffer,
            UntilFunction(MatchStringOrSize("\r\n\r\n", MAX_HEADER_SIZE - bytesRead)),
            [this, socket, buffer] (const boost::system::error_code& e, size_t /*size*/) mutable {
                if (!e) {
                    std::cout << "Done read header" << std::endl;
                } else {
                    std::cout << "error: " << e.message() << std::endl;
                }
            });
    }

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
};

}
