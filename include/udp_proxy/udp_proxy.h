#pragma once

#include <boost/asio.hpp>
#include <boost/asio/system_timer.hpp>

#include <iostream>
#include <experimental/string_view>

namespace UdpProxy {

using boost::asio::ip::tcp;
using namespace std::chrono_literals;

constexpr static size_t MAX_HEADER_SIZE = 4 * 1024;
constexpr static boost::asio::system_timer::duration HEADER_READ_TIMEOUT = 1s;

class Server {
public:
    Server(boost::asio::io_service &ioService)
            : acceptor(ioService, tcp::endpoint(tcp::v4(), 10013)) {
        startAccept();
    }

private:
    tcp::acceptor acceptor;

    typedef boost::asio::buffers_iterator<boost::asio::streambuf::const_buffers_type> UntilIterator;
    typedef std::function<std::pair<UntilIterator, bool>(UntilIterator begin, UntilIterator end) noexcept> UntilFunction;

    void startAccept() {
        auto socket = std::make_shared<tcp::socket>(acceptor.get_io_service());

        acceptor.async_accept(*socket, [this, socket] (const boost::system::error_code &e) mutable {
            if (!e) {
                HttpHeaderReader::read(socket);
            } else {
                std::cout << e.message() << std::endl;
            }

            startAccept();
        });
    }

    class HttpHeaderReader : public std::enable_shared_from_this<HttpHeaderReader> {
    public:
        static void read(std::shared_ptr<tcp::socket> &socket) {
            auto reader = std::shared_ptr<HttpHeaderReader>(new HttpHeaderReader(socket));
            reader->validateHttpMethod();
        }

        void validateHttpMethod() {
            constexpr static std::experimental::string_view REQUEST_METHOD_STRING("GET ", 4);

            timeoutTimer.expires_from_now(HEADER_READ_TIMEOUT);
            timeoutTimer.async_wait([this, capture = shared_from_this()] (const boost::system::error_code &e) {
                if (e != boost::system::errc::operation_canceled) {
                    socket->cancel();
                }
            });

            boost::asio::async_read_until(*socket, buffer,
                UntilFunction(MatchStringOrSize(REQUEST_METHOD_STRING, REQUEST_METHOD_STRING.size())),
                [this, capture = shared_from_this()] (const boost::system::error_code &e, size_t size) {
                    if (!e) {
                        std::experimental::string_view method(boost::asio::buffer_cast<const char*>(buffer.data()), REQUEST_METHOD_STRING.size());
                        if (REQUEST_METHOD_STRING == method) {
                            buffer.consume(size);
                            bytesRead += size;
                            readHttpRequestUri();
                        } else {
                            std::cout << "error: method not supported" << std::endl;
                        }
                    } else {
                        std::cout << "error: " << e.message() << std::endl;
                    }
                });
        }

        void readHttpRequestUri() {
            constexpr static std::experimental::string_view REQUEST_LINE_ENDING(" HTTP/1.1\r\n", 11);

            boost::asio::async_read_until(*socket, buffer,
                UntilFunction(MatchStringOrSize(REQUEST_LINE_ENDING, MAX_HEADER_SIZE - bytesRead)),
                [this, capture = shared_from_this()] (const boost::system::error_code &e, size_t size) {
                    if (!e) {
                        uri = {boost::asio::buffer_cast<const char*>(buffer.data()), size - REQUEST_LINE_ENDING.size()};
                        std::cout << "URI: '" << uri << '\'' << std::endl;
                        buffer.consume(size - 2); // Do not consume CRLF
                        bytesRead += (size - 2);
                        readRestOfHttpHeader();
                    } else {
                        std::cout << "error: " << e.message() << std::endl;
                    }
                });
        }

        void readRestOfHttpHeader() {
            boost::asio::async_read_until(*socket, buffer,
                UntilFunction(MatchStringOrSize("\r\n\r\n", MAX_HEADER_SIZE - bytesRead)),
                [this, capture = shared_from_this()] (const boost::system::error_code &e, size_t /*size*/) {
                    timeoutTimer.cancel();

                    if (!e) {
                        std::cout << "Done read header" << std::endl;
                    } else {
                        std::cout << "error: " << e.message() << std::endl;
                    }
                });
        }

    private:
        HttpHeaderReader(std::shared_ptr<tcp::socket> &socket) : socket(socket), timeoutTimer(socket->get_io_service()) {}

        std::shared_ptr<tcp::socket> socket;
        boost::asio::streambuf buffer{MAX_HEADER_SIZE};
        size_t bytesRead = 0;
        std::string uri;
        boost::asio::system_timer timeoutTimer;
    };

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
