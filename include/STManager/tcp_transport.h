#ifndef STMANAGER_TCP_TRANSPORT_HPP
#define STMANAGER_TCP_TRANSPORT_HPP

#include <STManager/sync.h>
#include <STManager/stmanager_export.h>

namespace STManager {

/** TCP implementation of ISyncTransport used by the built-in sync workflow. */
class STMANAGER_EXPORT TcpSyncTransport : public ISyncTransport {
public:
    TcpSyncTransport();
    ~TcpSyncTransport() override;

    /** Connect to device_info.host:device_info.port. */
    Status connect(const DeviceInfo& device_info) override;

    /** Close the current connection if one is open. */
    Status disconnect() override;

    /** Send one framed protocol message over the active connection. */
    Status send_message(const std::string& message) override;

    /** Receive one framed protocol message from the active connection. */
    Status receive_message(std::string* message) override;

    /** Send all bytes from in as one framed stream. */
    Status send_stream(std::istream& in) override;

    /** Receive one framed stream and write it to out. */
    Status receive_stream(std::ostream& out) override;

    /** Return the local endpoint assigned to the active connection. */
    Status local_endpoint(std::string* host, int* port) const;

private:
    int socket_fd_;
    std::string local_host_;
    int local_port_;
};

}  // namespace STManager

#endif
