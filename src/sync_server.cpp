#include <STManager/sync.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>

namespace STManager {
namespace {

volatile sig_atomic_t g_stop_server = 0;

void handle_sigint(int) {
    g_stop_server = 1;
}

class SignalScope {
public:
    SignalScope() : previous_handler_(signal(SIGINT, handle_sigint)) {
        g_stop_server = 0;
    }

    ~SignalScope() {
        signal(SIGINT, previous_handler_);
    }

private:
    void (*previous_handler_)(int);
};

Status make_io_error(const std::string& prefix) {
    std::ostringstream message_stream;
    message_stream << prefix << ": " << std::strerror(errno);
    return Status(StatusCode::kIoError, message_stream.str());
}

Status write_all(int socket_fd, const char* data, size_t size) {
    size_t sent_size = 0;
    while (sent_size < size) {
        const ssize_t write_size = send(socket_fd, data + sent_size, size - sent_size, 0);
        if (write_size < 0) {
            if (errno == EINTR) {
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
            if (errno == EINTR) {
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
    uint32_t payload_size = 0;
    const Status length_status = read_u32(socket_fd, &payload_size);
    if (!length_status.ok()) {
        return length_status;
    }

    std::string message;
    message.resize(payload_size);
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

class AdvertiseProcess {
public:
    AdvertiseProcess() : child_pid_(-1) {}

    Status start(const std::string& name, int port, const std::string& device_id) {
        if (child_pid_ > 0) {
            return Status::ok_status();
        }

        const pid_t child_pid = fork();
        if (child_pid < 0) {
            return make_io_error("fork failed for avahi publish");
        }

        if (child_pid == 0) {
            const std::string port_string = to_string(port);
            const std::string txt_value = std::string("id=") + device_id;
            execlp(
                "avahi-publish-service",
                "avahi-publish-service",
                name.c_str(),
                "_stmanager-sync._tcp",
                port_string.c_str(),
                txt_value.c_str(),
                static_cast<char*>(NULL));
            _exit(127);
        }

        child_pid_ = child_pid;
        return Status::ok_status();
    }

    void stop() {
        if (child_pid_ <= 0) {
            return;
        }

        kill(child_pid_, SIGTERM);
        waitpid(child_pid_, NULL, 0);
        child_pid_ = -1;
    }

    ~AdvertiseProcess() {
        stop();
    }

private:
    std::string to_string(int value) {
        std::ostringstream out;
        out << value;
        return out.str();
    }

    pid_t child_pid_;
};

Status handle_pair_request(
    int client_fd,
    ITrustedDeviceStore* trusted_store,
    const nlohmann::json& request_json,
    const ServerOptions& options) {
    const std::string remote_device_id = request_json.value("device_id", std::string());
    const std::string pairing_code = request_json.value("pairing_code", std::string());
    const bool remember_device = request_json.value("remember_device", true);

    if (remote_device_id.empty()) {
        return send_json_response(client_fd, "pair_response", false, "Missing device_id");
    }

    if (!options.pairing_code.empty() && pairing_code != options.pairing_code) {
        return send_json_response(client_fd, "pair_response", false, "Invalid pairing code");
    }

    if (remember_device && trusted_store != NULL) {
        const Status load_status = trusted_store->load();
        if (!load_status.ok()) {
            return load_status;
        }

        const Status trust_status = trusted_store->trust_device(remote_device_id);
        if (!trust_status.ok()) {
            return trust_status;
        }

        const Status save_status = trusted_store->save();
        if (!save_status.ok()) {
            return save_status;
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
        return send_json_response(client_fd, "auth_response", false, "Missing device_id");
    }

    if (trusted_store == NULL) {
        return send_json_response(client_fd, "auth_response", false, "Trusted device store is not configured");
    }

    const Status load_status = trusted_store->load();
    if (!load_status.ok()) {
        return load_status;
    }

    if (!trusted_store->is_trusted(remote_device_id)) {
        return send_json_response(client_fd, "auth_response", false, "Device is not trusted");
    }

    if (direction != "pull") {
        return send_json_response(client_fd, "auth_response", false, "Only pull direction is supported");
    }

    const Status response_status = send_json_response(client_fd, "auth_response", true, std::string());
    if (!response_status.ok()) {
        return response_status;
    }

    std::stringstream backup_stream(std::ios::in | std::ios::out | std::ios::binary);
    const Status backup_status = data_manager.backup(backup_stream);
    if (!backup_status.ok()) {
        return backup_status;
    }

    return send_framed_stream(client_fd, backup_stream);
}

Status handle_client_connection(
    int client_fd,
    const DataManager& data_manager,
    ITrustedDeviceStore* trusted_store,
    const ServerOptions& options) {
    std::string request_message;
    const Status receive_status = receive_framed_string(client_fd, &request_message);
    if (!receive_status.ok()) {
        return receive_status;
    }

    nlohmann::json request_json;
    try {
        request_json = nlohmann::json::parse(request_message);
    } catch (const std::exception& exception) {
        return Status(StatusCode::kSyncProtocolError, exception.what());
    }

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
}

std::string port_to_string(int port) {
    std::ostringstream out;
    out << port;
    return out.str();
}

Status create_listen_socket(const ServerOptions& options, int* listen_fd, int* bound_port) {
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
        setsockopt(candidate_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        if (bind(candidate_fd, addr->ai_addr, addr->ai_addrlen) != 0) {
            last_error = make_io_error("bind failed");
            close(candidate_fd);
            continue;
        }

        if (listen(candidate_fd, 8) != 0) {
            last_error = make_io_error("listen failed");
            close(candidate_fd);
            continue;
        }

        struct sockaddr_storage local_addr;
        socklen_t local_addr_len = sizeof(local_addr);
        if (getsockname(candidate_fd, reinterpret_cast<struct sockaddr*>(&local_addr), &local_addr_len) != 0) {
            last_error = make_io_error("getsockname failed");
            close(candidate_fd);
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

        *listen_fd = candidate_fd;
        *bound_port = actual_port;
        freeaddrinfo(resolved_addrs);
        return Status::ok_status();
    }

    freeaddrinfo(resolved_addrs);
    return last_error;
}

}  // namespace

Status run_sync_server(
    const DataManager& data_manager,
    const std::string& local_device_id,
    ITrustedDeviceStore* trusted_store,
    const ServerOptions& options,
    int* bound_port) {
    if (!data_manager.is_valid()) {
        return data_manager.last_status();
    }
    if (local_device_id.empty()) {
        return Status(StatusCode::kSyncProtocolError, "local_device_id cannot be empty");
    }
    if (bound_port == NULL) {
        return Status(StatusCode::kSyncProtocolError, "bound_port output cannot be null");
    }

    int listen_fd = -1;
    int actual_port = 0;
    const Status listen_status = create_listen_socket(options, &listen_fd, &actual_port);
    if (!listen_status.ok()) {
        return listen_status;
    }
    *bound_port = actual_port;

    AdvertiseProcess advertise_process;
    if (options.advertise) {
        const std::string advertise_name = options.advertise_name.empty()
            ? local_device_id
            : options.advertise_name;
        advertise_process.start(advertise_name, actual_port, local_device_id);
    }

    SignalScope signal_scope;
    while (!g_stop_server) {
        struct sockaddr_storage client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        const int client_fd = accept(
            listen_fd,
            reinterpret_cast<struct sockaddr*>(&client_addr),
            &client_addr_len);
        if (client_fd < 0) {
            if (errno == EINTR && g_stop_server) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            close(listen_fd);
            return make_io_error("accept failed");
        }

        const Status handle_status =
            handle_client_connection(client_fd, data_manager, trusted_store, options);
        close(client_fd);
        if (!handle_status.ok()) {
            close(listen_fd);
            return handle_status;
        }
    }

    close(listen_fd);
    return Status::ok_status();
}

}  // namespace STManager
