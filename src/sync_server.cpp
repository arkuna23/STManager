#include <STManager/sync.h>

#include "archive_stream.h"
#include "discovery_mdns.h"
#include "platform_compat.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <sstream>

namespace STManager {
namespace {

const uint32_t kMaxControlMessageBytes = 1024 * 1024;

class DiscoveryResponderScope {
public:
    DiscoveryResponderScope() : started_(false) {}

    Status start(
        const std::string& local_device_id,
        const std::string& local_device_name,
        const std::string& local_bind_host,
        int local_port) {
        if (started_) {
            return Status::ok_status();
        }

        DeviceInfo local_device;
        local_device.device_id = local_device_id;
        local_device.device_name = local_device_name;
        local_device.host = local_bind_host;
        local_device.port = local_port;

        const Status start_status = internal::start_discovery_responder(local_device);
        if (!start_status.ok()) {
            return start_status;
        }

        started_ = true;
        return Status::ok_status();
    }

    Status stop() {
        if (!started_) {
            return Status::ok_status();
        }

        const Status stop_status = internal::stop_discovery_responder();
        started_ = false;
        return stop_status;
    }

    ~DiscoveryResponderScope() {
        stop();
    }

private:
    bool started_;
};

Status make_io_error(const std::string& prefix) {
    std::ostringstream message_stream;
    message_stream << prefix << ": " << internal::socket_last_error_message();
    return Status(StatusCode::kIoError, message_stream.str());
}

int set_socket_option_int(int socket_fd, int level, int option_name, int option_value) {
#ifdef _WIN32
    return setsockopt(
        socket_fd,
        level,
        option_name,
        reinterpret_cast<const char*>(&option_value),
        static_cast<int>(sizeof(option_value)));
#else
    return setsockopt(socket_fd, level, option_name, &option_value, sizeof(option_value));
#endif
}

int wait_for_socket_readable(int socket_fd, int timeout_ms) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(socket_fd, &read_fds);

    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    return select(socket_fd + 1, &read_fds, NULL, NULL, &timeout);
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
    const uint32_t high_part = static_cast<uint32_t>((value >> 32) & 0xffffffffULL);
    const uint32_t low_part = static_cast<uint32_t>(value & 0xffffffffULL);

    const Status high_status = write_u32(socket_fd, high_part);
    if (!high_status.ok()) {
        return high_status;
    }

    return write_u32(socket_fd, low_part);
}

Status read_u64(int socket_fd, uint64_t* value) {
    uint32_t high_part = 0;
    uint32_t low_part = 0;

    const Status high_status = read_u32(socket_fd, &high_part);
    if (!high_status.ok()) {
        return high_status;
    }
    const Status low_status = read_u32(socket_fd, &low_part);
    if (!low_status.ok()) {
        return low_status;
    }

    *value = (static_cast<uint64_t>(high_part) << 32) | static_cast<uint64_t>(low_part);
    return Status::ok_status();
}

Status send_framed_string(int socket_fd, const std::string& payload) {
    if (payload.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        return Status(StatusCode::kSyncProtocolError, "Message is too large");
    }

    const Status length_status = write_u32(socket_fd, static_cast<uint32_t>(payload.size()));
    if (!length_status.ok()) {
        return length_status;
    }

    return write_all(socket_fd, payload.data(), payload.size());
}

Status receive_framed_string(int socket_fd, std::string* payload) {
    if (payload == NULL) {
        return Status(StatusCode::kSyncProtocolError, "payload output cannot be null");
    }

    uint32_t payload_size = 0;
    const Status length_status = read_u32(socket_fd, &payload_size);
    if (!length_status.ok()) {
        return length_status;
    }
    if (payload_size > kMaxControlMessageBytes) {
        return Status(
            StatusCode::kSyncProtocolError,
            "Control message too large");
    }

    std::string message;
    try {
        message.resize(payload_size);
    } catch (const std::exception& exception) {
        return Status(
            StatusCode::kIoError,
            std::string("stage=server.protocol.receive_framed_string.resize; ") + exception.what());
    } catch (...) {
        return Status(
            StatusCode::kIoError,
            "stage=server.protocol.receive_framed_string.resize; unknown exception");
    }
    if (payload_size > 0) {
        const Status read_status = read_all(socket_fd, &message[0], payload_size);
        if (!read_status.ok()) {
            return read_status;
        }
    }

    *payload = message;
    return Status::ok_status();
}

Status send_framed_stream(int socket_fd, std::istream& stream) {
    stream.clear();
    stream.seekg(0, std::ios::end);
    const std::streampos end_position = stream.tellg();
    if (end_position < 0) {
        return Status(StatusCode::kIoError, "Failed to determine stream size");
    }
    stream.seekg(0, std::ios::beg);

    const uint64_t stream_size = static_cast<uint64_t>(end_position);
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
        stream.read(buffer, chunk_size);
        const std::streamsize read_size = stream.gcount();
        if (read_size <= 0) {
            return Status(StatusCode::kIoError, "Unexpected end of stream");
        }

        const Status write_status = write_all(socket_fd, buffer, static_cast<size_t>(read_size));
        if (!write_status.ok()) {
            return write_status;
        }
        sent_size += static_cast<uint64_t>(read_size);
    }

    return Status::ok_status();
}

Status send_json_response(int socket_fd, const std::string& message_type, bool ok, const std::string& error) {
    nlohmann::json response_json;
    response_json["type"] = message_type;
    response_json["ok"] = ok;
    if (!error.empty()) {
        response_json["error"] = error;
    }
    return send_framed_string(socket_fd, response_json.dump());
}

std::string with_server_build_info(const std::string& message) {
    const std::string server_build = std::string(__DATE__) + " " + std::string(__TIME__);
    if (message.empty()) {
        return "server_build=" + server_build;
    }
    return "server_build=" + server_build + "; " + message;
}

std::string with_server_context(const std::string& stage, const std::string& message) {
    std::ostringstream message_stream;
    message_stream << with_server_build_info(std::string()) << "; stage=" << stage;
    if (!message.empty()) {
        message_stream << "; " << message;
    }
    return message_stream.str();
}

Status handle_pair_request(
    int client_fd,
    ITrustedDeviceStore* trusted_store,
    const nlohmann::json& request_json,
    const ServerOptions& options) {
    const std::string remote_device_id = request_json.value("device_id", std::string());
    const std::string pairing_code = request_json.value("pairing_code", std::string());
    const bool remember_device = request_json.value("remember_device", true);

    if (remote_device_id.empty()) {
        return send_json_response(
            client_fd,
            "pair_response",
            false,
            with_server_context("server.pair.validate.device_id", "Missing device_id"));
    }

    if (!options.pairing_code.empty() && pairing_code != options.pairing_code) {
        return send_json_response(
            client_fd,
            "pair_response",
            false,
            with_server_context("server.pair.validate.pairing_code", "Invalid pairing code"));
    }

    if (remember_device && trusted_store != NULL) {
        const Status load_status = trusted_store->load();
        if (!load_status.ok()) {
            return send_json_response(
                client_fd,
                "pair_response",
                false,
                with_server_context("server.pair.trusted_store.load", load_status.message));
        }

        const Status trust_status = trusted_store->trust_device(remote_device_id);
        if (!trust_status.ok()) {
            return send_json_response(
                client_fd,
                "pair_response",
                false,
                with_server_context("server.pair.trusted_store.trust", trust_status.message));
        }

        const Status save_status = trusted_store->save();
        if (!save_status.ok()) {
            return send_json_response(
                client_fd,
                "pair_response",
                false,
                with_server_context("server.pair.trusted_store.save", save_status.message));
        }
    }

    return send_json_response(client_fd, "pair_response", true, std::string());
}

Status handle_auth_request(
    int client_fd,
    const DataManager& data_manager,
    ITrustedDeviceStore* trusted_store,
    const nlohmann::json& request_json) {
    const std::string remote_device_id = request_json.value("device_id", std::string());
    const std::string direction = request_json.value("direction", std::string());

    if (remote_device_id.empty()) {
        return send_json_response(
            client_fd,
            "auth_response",
            false,
            with_server_context("server.auth.validate.device_id", "Missing device_id"));
    }

    if (trusted_store == NULL) {
        return send_json_response(
            client_fd,
            "auth_response",
            false,
            with_server_context(
                "server.auth.trusted_store.config",
                "Trusted device store is not configured"));
    }

    const Status load_status = trusted_store->load();
    if (!load_status.ok()) {
        return send_json_response(
            client_fd,
            "auth_response",
            false,
            with_server_context("server.auth.trusted_store.load", load_status.message));
    }

    if (!trusted_store->is_trusted(remote_device_id)) {
        return send_json_response(
            client_fd,
            "auth_response",
            false,
            with_server_context("server.auth.trusted_store.verify", "Device is not trusted"));
    }

    if (direction != "pull") {
        return send_json_response(
            client_fd,
            "auth_response",
            false,
            with_server_context(
                "server.auth.validate.direction",
                "Only pull direction is supported"));
    }

    std::stringstream backup_stream(std::ios::in | std::ios::out | std::ios::binary);
    const Status backup_status = data_manager.backup(backup_stream);
    if (!backup_status.ok()) {
        return send_json_response(
            client_fd,
            "auth_response",
            false,
            with_server_context("server.auth.backup.create", backup_status.message));
    }

    backup_stream.clear();
    backup_stream.seekp(0, std::ios::end);
    const std::streampos backup_end = backup_stream.tellp();
    if (backup_end <= 0) {
        return send_json_response(
            client_fd,
            "auth_response",
            false,
            with_server_context("server.auth.backup.empty", "Backup produced empty archive"));
    }

    const Status response_status = send_json_response(client_fd, "auth_response", true, std::string());
    if (!response_status.ok()) {
        return response_status;
    }

    backup_stream.clear();
    backup_stream.seekg(0, std::ios::beg);
    return send_framed_stream(client_fd, backup_stream);
}

Status handle_client_connection(
    int client_fd,
    const DataManager& data_manager,
    ITrustedDeviceStore* trusted_store,
    const ServerOptions& options) {
    try {
        std::string request_message;
        const Status receive_status = receive_framed_string(client_fd, &request_message);
        if (!receive_status.ok()) {
            return receive_status;
        }

        nlohmann::json request_json = nlohmann::json::parse(request_message);
        if (!request_json.is_object()) {
            return Status(StatusCode::kSyncProtocolError, "Protocol request must be a json object");
        }

        const std::string message_type = request_json.value("type", std::string());
        if (message_type == "pair_request") {
            return handle_pair_request(client_fd, trusted_store, request_json, options);
        }
        if (message_type == "auth_request") {
            return handle_auth_request(client_fd, data_manager, trusted_store, request_json);
        }

        return Status(StatusCode::kSyncProtocolError, "Unsupported request type");
    } catch (const std::exception& exception) {
        return Status(
            StatusCode::kSyncProtocolError,
            std::string("stage=server.client.exception; ") + exception.what());
    } catch (...) {
        return Status(
            StatusCode::kSyncProtocolError,
            "stage=server.client.exception; unknown exception");
    }
}

std::string port_to_string(int port) {
    std::ostringstream out;
    out << port;
    return out.str();
}

bool is_wildcard_bind_host(const std::string& host) {
    return host.empty() || host == "0.0.0.0" || host == "::" || host == "[::]";
}

bool sockaddr_to_ip(
    const struct sockaddr_storage& socket_addr,
    socklen_t socket_addr_len,
    std::string* ip_text) {
    if (ip_text == NULL) {
        return false;
    }

    char host_buffer[INET6_ADDRSTRLEN];
    std::memset(host_buffer, 0, sizeof(host_buffer));
    if (getnameinfo(
            reinterpret_cast<const struct sockaddr*>(&socket_addr),
            socket_addr_len,
            host_buffer,
            static_cast<socklen_t>(sizeof(host_buffer)),
            NULL,
            0,
            NI_NUMERICHOST) != 0) {
        return false;
    }

    *ip_text = host_buffer;
    return !ip_text->empty();
}

bool detect_local_reachable_ipv4(std::string* local_ip) {
    if (local_ip == NULL) {
        return false;
    }

    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd >= 0) {
        struct sockaddr_in remote_addr;
        std::memset(&remote_addr, 0, sizeof(remote_addr));
        remote_addr.sin_family = AF_INET;
        remote_addr.sin_port = htons(53);
        remote_addr.sin_addr.s_addr = htonl(0x08080808U);

        if (connect(socket_fd, reinterpret_cast<const struct sockaddr*>(&remote_addr), sizeof(remote_addr)) == 0) {
            struct sockaddr_storage local_addr;
            std::memset(&local_addr, 0, sizeof(local_addr));
            socklen_t local_addr_len = sizeof(local_addr);
            const int get_name_status = getsockname(
                socket_fd,
                reinterpret_cast<struct sockaddr*>(&local_addr),
                &local_addr_len);
            if (get_name_status == 0 &&
                sockaddr_to_ip(local_addr, local_addr_len, local_ip) &&
                !is_wildcard_bind_host(*local_ip) &&
                *local_ip != "127.0.0.1") {
                internal::close_socket_fd(socket_fd);
                return true;
            }
        }
        internal::close_socket_fd(socket_fd);
    }

#ifndef _WIN32
    struct ifaddrs* interface_list = NULL;
    if (getifaddrs(&interface_list) != 0 || interface_list == NULL) {
        return false;
    }

    for (struct ifaddrs* it = interface_list; it != NULL; it = it->ifa_next) {
        if (it->ifa_addr == NULL) {
            continue;
        }
        if (it->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((it->ifa_flags & IFF_UP) == 0) {
            continue;
        }
        if ((it->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        const struct sockaddr_in* ipv4_addr =
            reinterpret_cast<const struct sockaddr_in*>(it->ifa_addr);
        char ip_buffer[INET_ADDRSTRLEN];
        std::memset(ip_buffer, 0, sizeof(ip_buffer));
        if (inet_ntop(AF_INET, &ipv4_addr->sin_addr, ip_buffer, sizeof(ip_buffer)) == NULL) {
            continue;
        }

        const std::string candidate_ip = ip_buffer;
        if (candidate_ip.empty() || is_wildcard_bind_host(candidate_ip) ||
            candidate_ip == "127.0.0.1") {
            continue;
        }

        *local_ip = candidate_ip;
        freeifaddrs(interface_list);
        return true;
    }

    freeifaddrs(interface_list);
#endif
    return false;
}

Status create_listen_socket(
    const ServerOptions& options,
    int* listen_fd,
    int* bound_port,
    std::string* bound_host) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo* resolved_addrs = NULL;
    const std::string port_string = port_to_string(options.port);
    const int resolve_result = getaddrinfo(
        options.bind_host.c_str(),
        port_string.c_str(),
        &hints,
        &resolved_addrs);
    if (resolve_result != 0) {
        return Status(StatusCode::kIoError, gai_strerror(resolve_result));
    }

    Status last_error(StatusCode::kIoError, "Unable to create server socket");
    for (struct addrinfo* addr = resolved_addrs; addr != NULL; addr = addr->ai_next) {
        const int candidate_fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (candidate_fd < 0) {
            last_error = make_io_error("socket creation failed");
            continue;
        }

        int reuse = 1;
        set_socket_option_int(candidate_fd, SOL_SOCKET, SO_REUSEADDR, reuse);

        if (bind(candidate_fd, addr->ai_addr, addr->ai_addrlen) != 0) {
            last_error = make_io_error("bind failed");
            internal::close_socket_fd(candidate_fd);
            continue;
        }

        if (listen(candidate_fd, 8) != 0) {
            last_error = make_io_error("listen failed");
            internal::close_socket_fd(candidate_fd);
            continue;
        }

        struct sockaddr_storage local_addr;
        socklen_t local_addr_len = sizeof(local_addr);
        if (getsockname(candidate_fd, reinterpret_cast<struct sockaddr*>(&local_addr), &local_addr_len) != 0) {
            last_error = make_io_error("getsockname failed");
            internal::close_socket_fd(candidate_fd);
            continue;
        }

        int actual_port = 0;
        if (local_addr.ss_family == AF_INET) {
            struct sockaddr_in* ipv4_addr = reinterpret_cast<struct sockaddr_in*>(&local_addr);
            actual_port = ntohs(ipv4_addr->sin_port);
        } else if (local_addr.ss_family == AF_INET6) {
            struct sockaddr_in6* ipv6_addr = reinterpret_cast<struct sockaddr_in6*>(&local_addr);
            actual_port = ntohs(ipv6_addr->sin6_port);
        }

        std::string actual_host;
        if (!sockaddr_to_ip(local_addr, local_addr_len, &actual_host) || is_wildcard_bind_host(actual_host)) {
            actual_host = options.bind_host;
        }
        if (is_wildcard_bind_host(actual_host) && !detect_local_reachable_ipv4(&actual_host)) {
            actual_host = "127.0.0.1";
        }
        if (is_wildcard_bind_host(actual_host)) {
            actual_host = "127.0.0.1";
        }

        *listen_fd = candidate_fd;
        *bound_port = actual_port;
        if (bound_host != NULL) {
            *bound_host = actual_host;
        }
        freeaddrinfo(resolved_addrs);
        return Status::ok_status();
    }

    freeaddrinfo(resolved_addrs);
    return last_error;
}

}  // namespace

Status serve_sync_server(
    const DataManager& data_manager,
    const std::string& local_device_id,
    ITrustedDeviceStore* trusted_store,
    const ServerOptions& options,
    int* bound_port,
    std::string* bound_host,
    const std::atomic<bool>* stop_requested,
    ServeSyncStartedCallback started_callback,
    void* started_context) {
    if (!data_manager.is_valid()) {
        return data_manager.last_status();
    }
    if (local_device_id.empty()) {
        return Status(StatusCode::kSyncProtocolError, "local_device_id cannot be empty");
    }
    if (bound_port == NULL) {
        return Status(StatusCode::kSyncProtocolError, "bound_port output cannot be null");
    }
    if (bound_host != NULL) {
        bound_host->clear();
    }

    const Status runtime_status = internal::ensure_socket_runtime();
    if (!runtime_status.ok()) {
        return runtime_status;
    }

    int listen_fd = -1;
    int actual_port = 0;
    std::string actual_host;
    const Status listen_status =
        create_listen_socket(options, &listen_fd, &actual_port, &actual_host);
    if (!listen_status.ok()) {
        return listen_status;
    }
    *bound_port = actual_port;
    if (bound_host != NULL) {
        *bound_host = actual_host;
    }
    if (started_callback != NULL) {
        started_callback(actual_host, actual_port, started_context);
    }

    DiscoveryResponderScope discovery_responder_scope;
    if (options.advertise) {
        const std::string advertise_name = options.advertise_name.empty()
            ? local_device_id
            : options.advertise_name;
        const Status discovery_start_status = discovery_responder_scope.start(
            local_device_id,
            advertise_name,
            actual_host,
            actual_port);
        if (!discovery_start_status.ok()) {
            internal::close_socket_fd(listen_fd);
            return discovery_start_status;
        }
    }

    while (true) {
        if (stop_requested != NULL && stop_requested->load()) {
            break;
        }

        const int select_result = wait_for_socket_readable(listen_fd, 500);
        if (select_result < 0) {
            if (internal::socket_error_is_interrupt()) {
                continue;
            }
            internal::close_socket_fd(listen_fd);
            return make_io_error("select failed");
        }
        if (select_result == 0) {
            continue;
        }

        struct sockaddr_storage client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        const int client_fd = accept(
            listen_fd,
            reinterpret_cast<struct sockaddr*>(&client_addr),
            &client_addr_len);
        if (client_fd < 0) {
            if (stop_requested != NULL && stop_requested->load()) {
                break;
            }
            if (internal::socket_error_is_interrupt()) {
                continue;
            }
            continue;
        }

        const Status handle_status =
            handle_client_connection(client_fd, data_manager, trusted_store, options);
        internal::close_socket_fd(client_fd);
        (void)handle_status;
    }

    const Status discovery_stop_status = discovery_responder_scope.stop();
    internal::close_socket_fd(listen_fd);
    if (!discovery_stop_status.ok()) {
        return discovery_stop_status;
    }
    return Status::ok_status();
}

}  // namespace STManager
