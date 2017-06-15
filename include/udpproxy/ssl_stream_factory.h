#pragma once

#include <boost/asio/ssl.hpp>

namespace UdpProxy {

namespace ssl = boost::asio::ssl;

class SslStream {
public:
    typedef boost::asio::ssl::stream<tcp::socket> stream_type;
    typedef stream_type::lowest_layer_type lowest_layer_type;
    typedef stream_type::next_layer_type next_layer_type;

    SslStream(boost::asio::io_service& ioService, ssl::context& context, bool enableSsl)
            : sslStream(ioService, context), enableSsl(enableSsl) {
    }

    boost::asio::io_service& get_io_service() {
        return sslStream.get_io_service();
    }

    lowest_layer_type& lowest_layer() {
        return sslStream.lowest_layer();
    }

    stream_type& next_layer() {
        return sslStream;
    }

    template <typename ConstBufferSequence, typename WriteHandler>
    BOOST_ASIO_INITFN_RESULT_TYPE(WriteHandler, void (boost::system::error_code, std::size_t))
    async_write_some(const ConstBufferSequence& buffers, BOOST_ASIO_MOVE_ARG(WriteHandler) handler) {
        if (enableSsl) {
            return sslStream.async_write_some(buffers, handler);
        } else {
            return sslStream.next_layer().async_write_some(buffers, handler);
        }
    }

    template <typename MutableBufferSequence, typename ReadHandler>
    BOOST_ASIO_INITFN_RESULT_TYPE(ReadHandler, void (boost::system::error_code, std::size_t))
    async_read_some(const MutableBufferSequence& buffers, BOOST_ASIO_MOVE_ARG(ReadHandler) handler) {
        if (enableSsl) {
            sslStream.async_read_some(buffers, handler);
        } else {
            sslStream.next_layer().async_read_some(buffers, handler);
        }
    }

    template <typename HandshakeHandler>
    BOOST_ASIO_INITFN_RESULT_TYPE(HandshakeHandler, void (boost::system::error_code))
    async_handshake(stream_type::handshake_type type, BOOST_ASIO_MOVE_ARG(HandshakeHandler) handler) {
        if (enableSsl) {
            return sslStream.async_handshake(type, handler);
        } else {
            return sslStream.get_io_service().post([handler] () { handler(boost::system::error_code()); });
        }
    }

private:
    stream_type sslStream;
    bool enableSsl = true;
};

class SslStreamFactory {
public:
    typedef SslStream StreamType;

    std::shared_ptr<StreamType> create(boost::asio::io_service &ioService) {
        return std::make_shared<StreamType>(ioService, context, sslEnabled);
    }

    void startAsync(std::shared_ptr<StreamType> &stream, std::function<void(const boost::system::error_code& error)> handler) {
        stream->async_handshake(ssl::stream_base::server, handler);
    }

    ssl::context& getContext() noexcept { return context; }

    void enableSsl(bool enable) noexcept {
        sslEnabled = enable;
    }

private:
    bool sslEnabled = true;
    ssl::context context = ssl::context(ssl::context::sslv23);
};

}
