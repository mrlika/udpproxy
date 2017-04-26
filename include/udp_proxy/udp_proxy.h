#pragma once

#include <boost/asio.hpp>
#include <boost/asio/system_timer.hpp>

#include <iostream>
#include <regex>
#include <experimental/string_view>

namespace UdpProxy {

using boost::asio::ip::tcp;
using namespace std::chrono_literals;
using namespace std::experimental::string_view_literals;

static constexpr size_t MAX_HEADER_SIZE = 4 * 1024;
static constexpr boost::asio::system_timer::duration HEADER_READ_TIMEOUT = 1s;

class Server {
public:
    Server(boost::asio::io_service &ioService, const tcp::endpoint &endpoint)
            : acceptor(ioService, endpoint) {
        startAccept();
    }

private:
    class ServerError : public std::runtime_error {
    public:
        explicit ServerError(const std::string& message) : std::runtime_error(message) {}
        explicit ServerError(const char *message) : std::runtime_error(message) {}
    };

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
            auto reader = std::make_shared<HttpHeaderReader>(socket);
            reader->validateHttpMethod();
        }

        HttpHeaderReader(std::shared_ptr<tcp::socket> &socket) : socket(socket), timeoutTimer(socket->get_io_service()) {}

    private:
        void validateHttpMethod() {
            static constexpr std::experimental::string_view REQUEST_METHOD = "GET "sv;

            timeoutTimer.expires_from_now(HEADER_READ_TIMEOUT);
            timeoutTimer.async_wait([this, capture = shared_from_this()] (const boost::system::error_code &e) {
                if (e != boost::system::errc::operation_canceled) {
                    socket->cancel();
                }
            });

            boost::asio::async_read_until(*socket, buffer,
                UntilFunction(MatchStringOrSize(REQUEST_METHOD, REQUEST_METHOD.length())),
                [this, capture = shared_from_this()] (const boost::system::error_code &e, size_t size) {
                    try {
                        if (e) {
                            throw ServerError(e.message());
                        }

                        std::experimental::string_view method(boost::asio::buffer_cast<const char*>(buffer.data()), REQUEST_METHOD.length());
                        if (REQUEST_METHOD != method) {
                            throw ServerError("method not supported");
                        }

                        buffer.consume(size);
                        bytesRead += size;
                        readHttpRequestUri();
                    } catch (const ServerError &e) {
                        std::cout << "error: " << e.what() << std::endl;
                        timeoutTimer.cancel();
                    }
                });
        }

        void readHttpRequestUri() {
            static constexpr std::experimental::string_view HTTP_VERSION_ENDING = " HTTP/1.1\r\n"sv;
            static constexpr size_t MAX_REQUEST_LINE_SIZE = "/udp/ddd.ddd.ddd.ddd:ddddd"sv.length() + HTTP_VERSION_ENDING.length();
            static constexpr size_t MIN_REQUEST_LINE_SIZE = "/udp/d.d.d.d:d"sv.length() + HTTP_VERSION_ENDING.length();

            boost::asio::async_read_until(*socket, buffer,
                UntilFunction(MatchStringOrSize("\r\n", MAX_REQUEST_LINE_SIZE)),
                [this, capture = shared_from_this()] (const boost::system::error_code &e, size_t size) {
                    try {
                        if (e) {
                            throw ServerError(e.message());
                        } else if (size < MIN_REQUEST_LINE_SIZE) {
                            throw ServerError("request not supported");
                        }

                        std::experimental::string_view ending = {boost::asio::buffer_cast<const char*>(buffer.data()) + size - HTTP_VERSION_ENDING.length(), HTTP_VERSION_ENDING.length()};
                        if (HTTP_VERSION_ENDING != ending) {
                            throw ServerError("request not supported");
                        }

                        std::experimental::string_view uri = {boost::asio::buffer_cast<const char*>(buffer.data()), size - HTTP_VERSION_ENDING.length()};

                        static const std::regex uriRegex("/udp/(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}):(\\d{1,5})", std::regex_constants::optimize);
                        std::cmatch match;
                        std::regex_match(uri.begin(), uri.end(), match, uriRegex);

                        if (match.empty()) {
                            throw ServerError("wrong URI");
                        }

                        unsigned long portParsed;

                        try {
                            portParsed = std::stoul(match[2]);
                            address = boost::asio::ip::address::from_string(match[1]);
                        } catch (...) {
                            throw ServerError("wrong URI");
                        }

                        if ((portParsed == 0) || (portParsed > std::numeric_limits<unsigned short>::max())) {
                            throw ServerError("wrong URI");
                        }

                        port = portParsed;

                        buffer.consume(size - 2); // Do not consume CRLF
                        bytesRead += (size - 2);
                        readRestOfHttpHeader();
                    } catch (const ServerError &e) {
                        std::cout << "error: " << e.what() << std::endl;
                        timeoutTimer.cancel();
                    }
                });
        }

        void readRestOfHttpHeader() {
            boost::asio::async_read_until(*socket, buffer,
                UntilFunction(MatchStringOrSize("\r\n\r\n", MAX_HEADER_SIZE - bytesRead)),
                [this, capture = shared_from_this()] (const boost::system::error_code &e, size_t /*size*/) {
                    timeoutTimer.cancel();

                    if (e) {
                        std::cout << "error: " << e.message() << std::endl;
                        return;
                    }

                    std::cout << "Done read header: " + address.to_string() + ":" + std::to_string(port) << std::endl;
                });
        }

        std::shared_ptr<tcp::socket> socket;
        boost::asio::streambuf buffer{MAX_HEADER_SIZE};
        size_t bytesRead = 0;
        boost::asio::system_timer timeoutTimer;

        boost::asio::ip::address address;
        unsigned short port;
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
