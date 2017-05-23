#pragma once

#include <boost/asio.hpp>
#include <boost/asio/system_timer.hpp>

#include <iostream>
#include <unordered_map>
#include <list>

#include "version.h"

namespace UdpProxy {

using boost::asio::ip::tcp;
using boost::asio::ip::udp;
using namespace std::chrono_literals;

enum class OutputQueueOverflowAlgorithm {
    ClearQueue,
    DropData
};

template <typename Allocator, typename UdpInputCustomData, typename ClientCustomData>
class UdpToTcpProxyServer {
public:
    typedef std::function<void(const udp::endpoint &udpEndpoint, UdpInputCustomData &udpInputData) noexcept> NewUdpInputHandler;
    typedef std::function<void(const udp::endpoint &udpEndpoint, UdpInputCustomData &udpInputData) noexcept> RemoveUdpInputHandler;
    typedef std::function<void(const udp::endpoint &udpEndpoint, UdpInputCustomData &udpInputData) noexcept> StartUdpInputHandler;
    typedef std::function<void(const tcp::endpoint &clientEndpoint, const udp::endpoint &udpEndpoint, ClientCustomData &clientData, UdpInputCustomData &udpInputData) noexcept> NewClientHandler;
    typedef std::function<void(const tcp::endpoint &clientEndpoint, const udp::endpoint &udpEndpoint, ClientCustomData &clientData, UdpInputCustomData &udpInputData) noexcept> RemoveClientHandler;
    typedef std::function<void(const tcp::endpoint &clientEndpoint, const udp::endpoint &udpEndpoint, ClientCustomData &clientData, UdpInputCustomData &udpInputData) noexcept> StartClientHandler;
    typedef std::function<void(const udp::endpoint &udpEndpoint, size_t bytesRead, UdpInputCustomData &udpInputData) noexcept> ReadUdpInputHandler;
    typedef std::function<void(const tcp::endpoint &clientEndpoint, const udp::endpoint &udpEndpoint, size_t bytesWritten, ClientCustomData &clientData, UdpInputCustomData &udpInputData) noexcept> WriteClientHandler;

    struct ClientInfo {
        const tcp::endpoint& endpoint;
        ClientCustomData &customData;
        size_t outputQueueLength;
    };

    struct UdpInputInfo {
        const udp::endpoint &endpoint;
        UdpInputCustomData &customData;
        std::vector<ClientInfo> clients;
    };

    explicit UdpToTcpProxyServer(boost::asio::io_service &ioService) : ioService(ioService), clientsReadTimer(ioService) {
        static constexpr size_t CLIENT_READ_BUFFER_SIZE = 1024;
        clientsReadBuffer = std::make_shared<std::vector<uint8_t, InputBuffersAllocator>>(CLIENT_READ_BUFFER_SIZE);
    }

    void setVerboseLogging(bool value) noexcept { verboseLogging = value; }
    bool getVerboseLogging() const noexcept { return verboseLogging; }
    void setMaxUdpDataSize(size_t value) noexcept { maxUdpDataSize = value; }
    size_t getMaxUdpDataSize() const noexcept { return maxUdpDataSize; }
    void setMaxOutputQueueLength(size_t value) noexcept { maxOutputQueueLength = value; }
    size_t getMaxOutputQueueLength() const noexcept { return maxOutputQueueLength; }
    void setOutputQueueOverflowAlgorithm(OutputQueueOverflowAlgorithm value) noexcept { overflowAlgorithm = value; }
    OutputQueueOverflowAlgorithm getOutputQueueOverflowAlgorithm() const noexcept { return overflowAlgorithm; };
    void setRenewMulticastSubscriptionInterval(boost::asio::system_timer::duration value) noexcept { renewMulticastSubscriptionInterval = value; }
    boost::asio::system_timer::duration getRenewMulticastSubscriptionInterval() const noexcept { return renewMulticastSubscriptionInterval; }
    void setMulticastInterfaceAddress(boost::asio::ip::address value) noexcept { multicastInterfaceAddress = value; }
    boost::asio::ip::address getMulticastInterfaceAddress() const noexcept { return multicastInterfaceAddress; }

    void setNewUdpInputHandler(NewUdpInputHandler handler) noexcept { newUdpInputHandler = handler; }
    void setRemoveUdpInputHandler(RemoveUdpInputHandler handler) noexcept { removeUdpInputHandler = handler; }
    void setStartUdpInputHandler(StartUdpInputHandler handler) noexcept { startUdpInputHandler = handler; }
    void setNewClientHandler(NewClientHandler handler) noexcept { newClientHandler = handler; }
    void setRemoveClientHandler(RemoveClientHandler handler) noexcept { removeClientHandler = handler; }
    void setStartClientHandler(StartClientHandler handler) noexcept { startClientHandler = handler; }
    void setReadUdpInputHandler(ReadUdpInputHandler handler) noexcept { readUdpInputHandler = handler; }
    void setWriteClientHandler(WriteClientHandler handler) noexcept { writeClientHandler = handler; }

    NewUdpInputHandler getNewUdpInputHandler() const noexcept { return newUdpInputHandler; }
    RemoveUdpInputHandler getRemoveUdpInputHandler() const noexcept { return removeUdpInputHandler; }
    StartUdpInputHandler getStartUdpInputHandler() const noexcept { return startUdpInputHandler; }
    NewClientHandler getNewClientHandler() const noexcept { return newClientHandler; }
    RemoveClientHandler getRemoveClientHandler() const noexcept { return removeClientHandler; }
    StartClientHandler getStartClientHandler() const noexcept { return startClientHandler; }
    ReadUdpInputHandler getReadUdpInputHandler() const noexcept { return readUdpInputHandler; }
    WriteClientHandler getWriteClientHandler() const noexcept { return writeClientHandler; }

    void detectDisconnectedClientsAsync() {
        // Slowly read clients' sockets to detect disconnected ones
        startReadClients();
    }

    void addClient(const std::weak_ptr<tcp::socket> &clientSocket, const udp::endpoint &udpEndpoint, const ClientCustomData& clientData) {
        uint64_t inputId = getEndpointId(udpEndpoint);

        auto udpInputIterator = udpInputs.find(inputId);
        UdpInput *udpInput;

        if (udpInputIterator == udpInputs.end()) {
            std::unique_ptr<UdpInput> udpInputUnique;

            udpInputUnique = std::make_unique<UdpInput>(*this, inputId, udpEndpoint);

            udpInput = udpInputUnique.get();
            udpInputs.emplace(inputId, std::move(udpInputUnique));
        } else {
            udpInput = udpInputIterator->second.get();
        }

        udpInput->addClient(clientSocket, clientData);
    }

    void startClient(const tcp::endpoint &clientEndpoint, const udp::endpoint &udpEndpoint) {
        auto udpInputIterator = udpInputs.find(getEndpointId(udpEndpoint));
        if (udpInputIterator == udpInputs.end()) {
            return;
        }

        auto& clients = udpInputIterator->second->clients;

        auto it = std::find_if(clients.begin(), clients.end(), [&clientEndpoint] (const std::shared_ptr<typename UdpInput::Client> &client) { return client->remoteEndpoint == clientEndpoint; });
        if (it == clients.end()) {
            return;
        }

        try {
            (*it)->start();
        } catch (...) {
            removeClient(it->get());
            throw;
        }
    }

    void removeClient(const tcp::endpoint &clientEndpoint, const udp::endpoint &udpEndpoint) noexcept {
        auto udpInputIterator = udpInputs.find(getEndpointId(udpEndpoint));
        if (udpInputIterator == udpInputs.end()) {
            return;
        }

        auto& clients = udpInputIterator->second->clients;

        auto it = std::find_if(clients.begin(), clients.end(), [&clientEndpoint] (const std::shared_ptr<typename UdpInput::Client> &client) { return client->remoteEndpoint == clientEndpoint; });
        if (it == clients.end()) {
            return;
        }

        clients.erase(it);

        if (clients.empty()) {
            udpInputs.erase(udpInputIterator);
        }
    }

    std::vector<UdpInputInfo> getUdpInputs() const {
        std::vector<UdpInputInfo> result;
        result.reserve(udpInputs.size());

        for (auto& udpInput : udpInputs) {
            std::vector<ClientInfo> clients;
            clients.reserve(udpInput.second->clients.size());

            for (auto& client : udpInput.second->clients) {
                clients.emplace_back(ClientInfo{client->remoteEndpoint, client->customData, client->outputBuffers.size()});
            }

            result.emplace_back(UdpInputInfo{udpInput.second->udpEndpoint, udpInput.second->customData, std::move(clients)});
        }
        return result;
    }

private:
    typedef typename std::allocator_traits<Allocator>::template rebind_alloc<std::vector<uint8_t>> InputBuffersAllocator;

    struct UdpInput : public std::enable_shared_from_this<UdpToTcpProxyServer::UdpInput> {
        UdpInput(UdpToTcpProxyServer &server, uint64_t id, const udp::endpoint &udpEndpoint)
                : server(server), id(id), udpSocket(server.ioService),
                  udpEndpoint(udpEndpoint), renewMulticastSubscriptionTimer(server.ioService) {
            udpSocket.open(udpEndpoint.protocol());
            udpSocket.set_option(udp::socket::reuse_address(true)); // FIXME: is it good?
            udpSocket.bind(udpEndpoint);

            if (udpEndpoint.address().is_multicast()) {
                udpSocket.set_option(boost::asio::ip::multicast::join_group(udpEndpoint.address().to_v4(), server.multicastInterfaceAddress.to_v4()));
            }

            if (server.verboseLogging) {
                std::cerr << "new UDP input: udp://" << udpEndpoint << std::endl;
            }

            if (server.newUdpInputHandler) {
                server.newUdpInputHandler(udpEndpoint, customData);
            }
        }

        ~UdpInput() noexcept {
            if (server.verboseLogging) {
                std::cerr << "remove UDP input: " << udpEndpoint << std::endl;
            }

            if (server.removeUdpInputHandler) {
                server.removeUdpInputHandler(udpEndpoint, customData);
            }
        }

        void startRenewMulticastSubscription() {
            renewMulticastSubscriptionTimer.expires_from_now(server.renewMulticastSubscriptionInterval);
            renewMulticastSubscriptionTimer.async_wait([this, reference = std::weak_ptr<UdpInput>(this->shared_from_this())] (const boost::system::error_code &e) {
                if (reference.expired()) {
                    return;
                } else if (e) {
                    return;
                }

                try {
                    if (server.verboseLogging) {
                        std::cerr << "renew multicast subscription for " << udpEndpoint << std::endl;
                    }
                    udpSocket.set_option(boost::asio::ip::multicast::leave_group(udpEndpoint.address()));
                    udpSocket.set_option(boost::asio::ip::multicast::join_group(udpEndpoint.address()));
                } catch (const boost::system::system_error &e) {
                    std::cerr << "error: failed to renew multicast subscription for " << udpEndpoint << ": " << e.what() << std::endl;
                    server.udpInputs.erase(id);
                    return;
                }

                startRenewMulticastSubscription();
            });
        }

        void start() {
            if (isStarted) {
                return;
            }

            isStarted = true;

            if (udpEndpoint.address().is_multicast() && (server.renewMulticastSubscriptionInterval != 0s)) {
                startRenewMulticastSubscription();
            }

            if (server.startUdpInputHandler) {
                server.startUdpInputHandler(udpEndpoint, customData);
            }

            receiveUdp();
        }

        void receiveUdp() {
            inputBuffer = std::make_shared<std::vector<uint8_t, InputBuffersAllocator>>(server.maxUdpDataSize);

            udpSocket.async_receive_from(boost::asio::buffer(inputBuffer->data(), inputBuffer->size()), senderEndpoint,
                [this, reference = std::weak_ptr<UdpInput>(this->shared_from_this()), buffer = inputBuffer] (const boost::system::error_code &e, std::size_t bytesRead) {
                    if (reference.expired()) {
                        return;
                    }

                    if (e) {
                        std::cerr << "UDP socket receive error for " << udpEndpoint << ": " << e.message() << std::endl;
                        server.udpInputs.erase(id);
                        return;
                    }

                    if (server.readUdpInputHandler) {
                        server.readUdpInputHandler(udpEndpoint, bytesRead, customData);
                    }

                    inputBuffer->resize(bytesRead);

                    for (auto& client : clients) {
                        if (!client->isStarted) {
                            continue;
                        }

                        size_t length = client->outputBuffers.size();

                        if (length == 0) {
                            client->outputBuffers.emplace_back(inputBuffer);
                            client->startWriteData(inputBuffer);
                        } else if ((server.maxOutputQueueLength == 0) || (length < server.maxOutputQueueLength)) {
                            client->outputBuffers.emplace_back(inputBuffer);
                        } else {
                            switch (server.overflowAlgorithm) {
                            case OutputQueueOverflowAlgorithm::ClearQueue:
                                if (server.verboseLogging) {
                                    std::cerr << "error: output queue overflow - clearing queue for " << client->remoteEndpoint << " (udp://" << udpEndpoint << ")" << std::endl;
                                }
                                client->outputBuffers.resize(1);
                                break;

                            case OutputQueueOverflowAlgorithm::DropData:
                                if (server.verboseLogging) {
                                    std::cerr << "error: output queue overflow - dropping data for " << client->remoteEndpoint << " (udp://" << udpEndpoint << ")" << std::endl;
                                }
                                break;
                            }
                        }
                    }

                    receiveUdp();
                });
        }

        void addClient(const std::weak_ptr<tcp::socket>& socket, const ClientCustomData& clientData) {
            auto client = std::make_shared<typename UdpInput::Client>(socket, *this, clientData);
            clients.emplace_back(client);
        }

        struct Client : public std::enable_shared_from_this<UdpInput::Client> {
            Client(const std::weak_ptr<tcp::socket> &socket, UdpInput &udpInput, const ClientCustomData& customData) noexcept
                    : socket(socket), server(udpInput.server), udpInput(udpInput), remoteEndpoint(socket.lock()->remote_endpoint()), customData(customData) {
                if (server.verboseLogging) {
                    std::cerr << "new TCP client " << remoteEndpoint << " for " << udpInput.udpEndpoint <<  std::endl;
                }

                if (server.newClientHandler) {
                    server.newClientHandler(remoteEndpoint, udpInput.udpEndpoint, this->customData, udpInput.customData);
                }
            }

            ~Client() noexcept {
                if (server.verboseLogging) {
                    std::cerr << "remove TCP client " << remoteEndpoint << " for " << udpInput.udpEndpoint << std::endl;
                }

                if (server.removeClientHandler) {
                    server.removeClientHandler(remoteEndpoint, udpInput.udpEndpoint, customData, udpInput.customData);
                }
            }

            void startWriteData(std::shared_ptr<std::vector<uint8_t, InputBuffersAllocator>> &buffer) {
                if (socket.expired()) {
                    server.removeClient(this);
                    return;
                }

                boost::asio::async_write(*socket.lock(), boost::asio::buffer(buffer->data(), buffer->size()),
                    [this, reference = std::weak_ptr<Client>(this->shared_from_this()), buffer = buffer] (const boost::system::error_code &e, std::size_t bytesSent) {
                        if (reference.expired()) {
                            return;
                        }

                        if (e) {
                            if (server.verboseLogging) {
                                std::cerr << "write error for " << remoteEndpoint << ": " << e.message() << std::endl;
                            }
                            server.removeClient(this);
                            return;
                        }

                        if (server.writeClientHandler) {
                            server.writeClientHandler(remoteEndpoint, udpInput.udpEndpoint, bytesSent, customData, udpInput.customData);
                        }

                        assert(buffer == outputBuffers.front());
                        (void)bytesSent; // Avoid unused parameter warning when asserts disabled
                        assert(bytesSent == outputBuffers.front()->size());

                        outputBuffers.pop_front();
                        if (!outputBuffers.empty()) {
                            startWriteData(outputBuffers.front());
                        }
                    });
            }

            void doReadCheck() {
                if (!readSomeDone) {
                    return;
                }

                readSomeDone = false;
                socket.lock()->async_read_some(boost::asio::buffer(server.clientsReadBuffer->data(), server.clientsReadBuffer->size()),
                    [this, reference = std::weak_ptr<Client>(this->shared_from_this()), buffer = server.clientsReadBuffer] (const boost::system::error_code &e, std::size_t /*bytesRead*/) mutable {
                        if (reference.expired()) {
                            return;
                        }

                        if (e) {
                            if (server.verboseLogging) {
                                std::cerr << "error reading client " << remoteEndpoint << ": " << e.message() << std::endl;
                            }
                            server.removeClient(this);
                        }

                        readSomeDone = true;
                     });
            }

            void start() {
                udpInput.start();
                isStarted = true;
                if (server.startClientHandler) {
                    server.startClientHandler(remoteEndpoint, udpInput.udpEndpoint, customData, udpInput.customData);
                }
            }

            std::weak_ptr<tcp::socket> socket;
            UdpToTcpProxyServer &server;
            UdpInput &udpInput;
            tcp::endpoint remoteEndpoint;
            std::list<std::shared_ptr<std::vector<uint8_t, InputBuffersAllocator>>> outputBuffers;
            bool readSomeDone = true;
            bool isStarted = false;
            ClientCustomData customData;
        };

        UdpToTcpProxyServer &server;
        uint64_t id;
        std::list<std::shared_ptr<UdpInput::Client>> clients;
        udp::socket udpSocket;
        udp::endpoint senderEndpoint;
        udp::endpoint udpEndpoint;
        std::shared_ptr<std::vector<uint8_t, InputBuffersAllocator>> inputBuffer;
        bool isStarted = false;
        boost::asio::system_timer renewMulticastSubscriptionTimer;
        UdpInputCustomData customData;
    };

    static uint64_t getEndpointId(const udp::endpoint &udpEndpoint) noexcept {
        return (static_cast<uint64_t>(udpEndpoint.address().to_v4().to_ulong()) << 16) | udpEndpoint.port();
    }

    void removeClient(const typename UdpInput::Client *clientPointer) noexcept {
        auto udpInputIterator = udpInputs.find(clientPointer->udpInput.id);
        assert(udpInputIterator != udpInputs.end());

        auto& clients = udpInputIterator->second->clients;

        auto it = std::find_if(clients.begin(), clients.end(), [clientPointer] (const std::shared_ptr<typename UdpInput::Client> &client) { return client.get() == clientPointer; });
        assert(it != clients.end());
        clients.erase(it);

        if (clients.empty()) {
            udpInputs.erase(udpInputIterator);
        }
    }

    void startReadClients() {
        static constexpr boost::asio::system_timer::duration CLIENT_READ_PERIOD = 5s;

        clientsReadTimer.expires_from_now(CLIENT_READ_PERIOD);
        clientsReadTimer.async_wait([this] (const boost::system::error_code &e) {
            if (e == boost::system::errc::operation_canceled) {
                return;
            }

            for (auto udpInputIt = udpInputs.begin(); udpInputIt != udpInputs.end();) {
                auto& clients = udpInputIt->second->clients;
                for (auto clientIt = clients.begin(); clientIt != clients.end();) {
                    auto& client = *clientIt->get();
                    if (!client.socket.expired()) {
                        client.doReadCheck();
                        ++clientIt;
                    } else {
                        clientIt = clients.erase(clientIt);
                    }
                }

                if (clients.empty()) {
                    udpInputIt = udpInputs.erase(udpInputIt);
                } else {
                    ++udpInputIt;
                }
            }

            startReadClients();
        });
    }

    std::unordered_map<uint64_t, std::shared_ptr<UdpInput>> udpInputs;
    boost::asio::io_service &ioService;
    std::shared_ptr<std::vector<uint8_t, InputBuffersAllocator>> clientsReadBuffer;
    boost::asio::system_timer clientsReadTimer;

    bool verboseLogging = true;
    size_t maxUdpDataSize = 4 * 1024;
    size_t maxOutputQueueLength = 1024;
    OutputQueueOverflowAlgorithm overflowAlgorithm = OutputQueueOverflowAlgorithm::ClearQueue;
    boost::asio::system_timer::duration renewMulticastSubscriptionInterval = 0s;
    boost::asio::ip::address multicastInterfaceAddress;

    NewUdpInputHandler newUdpInputHandler;
    RemoveUdpInputHandler removeUdpInputHandler;
    StartUdpInputHandler startUdpInputHandler;
    NewClientHandler newClientHandler;
    RemoveClientHandler removeClientHandler;
    StartClientHandler startClientHandler;
    ReadUdpInputHandler readUdpInputHandler;
    WriteClientHandler writeClientHandler;
};

}
