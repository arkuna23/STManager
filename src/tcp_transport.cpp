#include <STManager/tcp_transport.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstring>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

#include "platform_compat.h"

namespace STManager {
namespace {

Status make_io_error(const std::string& prefix) {
    std::ostringstream message_stream;
    message_stream << prefix << ": " << internal::socket_last_error_message();
    return Status(StatusCode::kIoError, message_stream.str());
}

Status write_all(int socket_fd, const char* data, size_t size) {
    size_t sent_size = 0;
    while (sent_size < size) {
        const ssize_t write_size = send(socket_fd, data + sent_size, size - sent_size, 0);
        if (write_size < 0) {
            if (internal::socket_error_is_interrupt()) {
                continue;
            }
            return make_io_error("Socket write failed");
        }
        if (write_size == 0) {
            return Status(StatusCode::kIoError, "Socket closed during write");
        }
        sent_size += static_cast<size_t>(write_size);
    }
    return Status::ok_status();
}

Status read_all(int socket_fd, char* data, size_t size) {
    size_t received_size = 0;
    while (received_size < size) {
        const ssize_t read_size = recv(socket_fd, data + received_size, size - received_size, 0);
        if (read_size < 0) {
            if (internal::socket_error_is_interrupt()) {
                continue;
            }
            return make_io_error("Socket read failed");
        }
        if (read_size == 0) {
            return Status(StatusCode::kIoError, "Socket closed during read");
        }
        received_size += static_cast<size_t>(read_size);
    }
    return Status::ok_status();
}

Status write_u32(int socket_fd, uint32_t value) {
    const uint32_t network_value = htonl(value);
    return write_all(socket_fd, reinterpret_cast<const char*>(&network_value), sizeof(network_value));
}

Status read_u32(int socket_fd, uint32_t* value) {
    uint32_t network_value = 0;
    const Status read_status = read_all(socket_fd, reinterpret_cast<char*>(&network_value), sizeof(network_value));
    if (!read_status.ok()) {
        return read_status;
    }
    *value = ntohl(network_value);
    return Status::ok_status();
}

Status write_u64(int socket_fd, uint64_t value) {
    uint32_t high_part = static_cast<uint32_t>((value >> 32) & 0xffffffffULL);
    uint32_t low_part = static_cast<uint32_t>(value & 0xffffffffULL);

    const Status write_high_status = write_u32(socket_fd, high_part);
    if (!write_high_status.ok()) {
        return write_high_status;
    }
    return write_u32(socket_fd, low_part);
}

Status read_u64(int socket_fd, uint64_t* value) {
    uint32_t high_part = 0;
    uint32_t low_part = 0;
    const Status read_high_status = read_u32(socket_fd, &high_part);
    if (!read_high_status.ok()) {
        return read_high_status;
    }
    const Status read_low_status = read_u32(socket_fd, &low_part);
    if (!read_low_status.ok()) {
        return read_low_status;
    }

    *value = (static_cast<uint64_t>(high_part) << 32) | static_cast<uint64_t>(low_part);
    return Status::ok_status();
}

Status send_framed_string(int socket_fd, const std::string& message) {
    if (message.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        return Status(StatusCode::kIoError, "Message is too large");
    }

    const Status write_length_status = write_u32(socket_fd, static_cast<uint32_t>(message.size()));
    if (!write_length_status.ok()) {
        return write_length_status;
    }
    return write_all(socket_fd, message.data(), message.size());
}

Status receive_framed_string(int socket_fd, std::string* message) {
    uint32_t message_size = 0;
    const Status read_length_status = read_u32(socket_fd, &message_size);
    if (!read_length_status.ok()) {
        return read_length_status;
    }

    std::string buffer;
    buffer.resize(message_size);
    if (message_size == 0) {
        *message = buffer;
        return Status::ok_status();
    }

    const Status read_status = read_all(socket_fd, &buffer[0], message_size);
    if (!read_status.ok()) {
        return read_status;
    }

    *message = buffer;
    return Status::ok_status();
}

Status send_framed_stream(int socket_fd, std::istream& in) {
    in.clear();
    in.seekg(0, std::ios::end);
    const std::streampos stream_end = in.tellg();
    if (stream_end < 0) {
        return Status(StatusCode::kIoError, "Failed to determine stream length");
    }
    in.seekg(0, std::ios::beg);

    const uint64_t stream_size = static_cast<uint64_t>(stream_end);
    const Status write_size_status = write_u64(socket_fd, stream_size);
    if (!write_size_status.ok()) {
        return write_size_status;
    }

    char buffer[8192];
    uint64_t sent_size = 0;
    while (sent_size < stream_size) {
        const uint64_t remaining_size = stream_size - sent_size;
        const std::streamsize chunk_size = static_cast<std::streamsize>(
            remaining_size < sizeof(buffer) ? remaining_size : sizeof(buffer));

        in.read(buffer, chunk_size);
        const std::streamsize read_size = in.gcount();
        if (read_size <= 0) {
            return Status(StatusCode::kIoError, "Unexpected end of stream during send");
        }

        const Status write_status = write_all(socket_fd, buffer, static_cast<size_t>(read_size));
        if (!write_status.ok()) {
            return write_status;
        }
        sent_size += static_cast<uint64_t>(read_size);
    }

    return Status::ok_status();
}

Status receive_framed_stream(int socket_fd, std::ostream& out) {
    uint64_t stream_size = 0;
    const Status read_size_status = read_u64(socket_fd, &stream_size);
    if (!read_size_status.ok()) {
        return read_size_status;
    }

    char buffer[8192];
    uint64_t received_size = 0;
    while (received_size < stream_size) {
        const uint64_t remaining_size = stream_size - received_size;
        const size_t chunk_size = remaining_size < sizeof(buffer)
            ? static_cast<size_t>(remaining_size)
            : sizeof(buffer);

        const Status read_status = read_all(socket_fd, buffer, chunk_size);
        if (!read_status.ok()) {
            return read_status;
        }

        out.write(buffer, static_cast<std::streamsize>(chunk_size));
        if (!out) {
            return Status(StatusCode::kIoError, "Failed writing received stream data");
        }
        received_size += static_cast<uint64_t>(chunk_size);
    }

    return Status::ok_status();
}

std::string int_to_string(int value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

}  // namespace

TcpSyncTransport::TcpSyncTransport() : socket_fd_(-1), local_host_(), local_port_(0) {}

TcpSyncTransport::~TcpSyncTransport() {
    disconnect();
}

Status TcpSyncTransport::connect(const DeviceInfo& device_info) {
    disconnect();

    const Status runtime_status = internal::ensure_socket_runtime();
    if (!runtime_status.ok()) {
        return runtime_status;
    }

    if (device_info.host.empty() || device_info.port <= 0) {
        return Status(StatusCode::kSyncProtocolError, "Device host or port is invalid");
    }

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* resolved_addrs = NULL;
    const std::string port_string = int_to_string(device_info.port);
    const int resolve_result = getaddrinfo(
        device_info.host.c_str(),
        port_string.c_str(),
        &hints,
        &resolved_addrs);
    if (resolve_result != 0) {
        return Status(StatusCode::kIoError, gai_strerror(resolve_result));
    }

    Status last_error(StatusCode::kIoError, "Unable to connect to remote device");
    for (struct addrinfo* addr = resolved_addrs; addr != NULL; addr = addr->ai_next) {
        const int candidate_fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (candidate_fd < 0) {
            last_error = make_io_error("Socket creation failed");
            continue;
        }

        if (::connect(candidate_fd, addr->ai_addr, addr->ai_addrlen) == 0) {
            socket_fd_ = candidate_fd;

            struct sockaddr_storage local_addr;
            std::memset(&local_addr, 0, sizeof(local_addr));
            socklen_t local_addr_len = sizeof(local_addr);
            if (getsockname(
                    socket_fd_,
                    reinterpret_cast<struct sockaddr*>(&local_addr),
                    &local_addr_len) == 0) {
                char host_buffer[INET6_ADDRSTRLEN];
                std::memset(host_buffer, 0, sizeof(host_buffer));
                if (getnameinfo(
                        reinterpret_cast<const struct sockaddr*>(&local_addr),
                        local_addr_len,
                        host_buffer,
                        static_cast<socklen_t>(sizeof(host_buffer)),
                        NULL,
                        0,
                        NI_NUMERICHOST) == 0) {
                    local_host_ = host_buffer;
                } else {
                    local_host_.clear();
                }

                if (local_addr.ss_family == AF_INET) {
                    const struct sockaddr_in* ipv4_addr =
                        reinterpret_cast<const struct sockaddr_in*>(&local_addr);
                    local_port_ = static_cast<int>(ntohs(ipv4_addr->sin_port));
                } else if (local_addr.ss_family == AF_INET6) {
                    const struct sockaddr_in6* ipv6_addr =
                        reinterpret_cast<const struct sockaddr_in6*>(&local_addr);
                    local_port_ = static_cast<int>(ntohs(ipv6_addr->sin6_port));
                } else {
                    local_port_ = 0;
                }
            } else {
                local_host_.clear();
                local_port_ = 0;
            }

            freeaddrinfo(resolved_addrs);
            return Status::ok_status();
        }

        last_error = make_io_error("Socket connect failed");
        internal::close_socket_fd(candidate_fd);
    }

    freeaddrinfo(resolved_addrs);
    return last_error;
}

Status TcpSyncTransport::disconnect() {
    if (socket_fd_ >= 0) {
        internal::close_socket_fd(socket_fd_);
        socket_fd_ = -1;
    }
    return Status::ok_status();
}

Status TcpSyncTransport::local_endpoint(std::string* host, int* port) const {
    if (host == NULL || port == NULL) {
        return Status(StatusCode::kSyncProtocolError, "host and port outputs cannot be null");
    }

    *host = local_host_;
    *port = local_port_;
    return Status::ok_status();
}

Status TcpSyncTransport::send_message(const std::string& message) {
    if (socket_fd_ < 0) {
        return Status(StatusCode::kSyncProtocolError, "Transport is not connected");
    }
    return send_framed_string(socket_fd_, message);
}

Status TcpSyncTransport::receive_message(std::string* message) {
    if (socket_fd_ < 0) {
        return Status(StatusCode::kSyncProtocolError, "Transport is not connected");
    }
    if (message == NULL) {
        return Status(StatusCode::kSyncProtocolError, "message output cannot be null");
    }
    return receive_framed_string(socket_fd_, message);
}

Status TcpSyncTransport::send_stream(std::istream& in) {
    if (socket_fd_ < 0) {
        return Status(StatusCode::kSyncProtocolError, "Transport is not connected");
    }
    return send_framed_stream(socket_fd_, in);
}

Status TcpSyncTransport::receive_stream(std::ostream& out) {
    if (socket_fd_ < 0) {
        return Status(StatusCode::kSyncProtocolError, "Transport is not connected");
    }
    return receive_framed_stream(socket_fd_, out);
}

}  // namespace STManager
