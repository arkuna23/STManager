#ifndef STMANAGER_TCP_TRANSPORT_HPP
#define STMANAGER_TCP_TRANSPORT_HPP

#include <STManager/sync.h>
#include <STManager/stmanager_export.h>

namespace STManager {

class STMANAGER_EXPORT TcpSyncTransport : public ISyncTransport {
public:
    TcpSyncTransport();
    ~TcpSyncTransport() override;

    Status connect(const DeviceInfo& device_info) override;
    Status disconnect() override;

    Status send_message(const std::string& message) override;
    Status receive_message(std::string* message) override;

    Status send_stream(std::istream& in) override;
    Status receive_stream(std::ostream& out) override;

private:
    int socket_fd_;
};

}  // namespace STManager

#endif
