#include "discovery_mdns.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace STManager {
namespace internal {
namespace {

const int kDiscoveryPort = 38592;
const int kDiscoveryTimeoutMs = 1200;
const int kDiscoverySelectIntervalMs = 150;
const int kDiscoveryResponderPollMs = 500;
const char* kDiscoveryProtocol = "stmanager.discovery.v1";
const char* kDiscoverRequestType = "discover_request";
const char* kDiscoverResponseType = "discover_response";

std::atomic<bool> g_responder_running(false);
std::atomic<bool> g_responder_stop_requested(false);
std::thread g_responder_thread;
std::mutex g_responder_mutex;
int g_responder_socket_fd = -1;

Status make_io_error(const std::string& prefix) {
    std::ostringstream message_stream;
    message_stream << prefix << ": " << std::strerror(errno);
    return Status(StatusCode::kDiscoveryError, message_stream.str());
}

bool parse_discovery_response_message(
    const std::string& message,
    const std::string& fallback_host,
    DeviceInfo* device_info) {
    try {
        const nlohmann::json response_json = nlohmann::json::parse(message);
        if (!response_json.is_object()) {
            return false;
        }

        if (response_json.value("type", std::string()) != kDiscoverResponseType) {
            return false;
        }
        if (response_json.value("protocol", std::string()) != kDiscoveryProtocol) {
            return false;
        }

        device_info->device_id = response_json.value("device_id", std::string());
        device_info->device_name = response_json.value("device_name", std::string());
        device_info->host = response_json.value("host", std::string());
        device_info->port = response_json.value("port", 0);

        if (device_info->host.empty()) {
            device_info->host = fallback_host;
        }
        if (device_info->device_name.empty()) {
            device_info->device_name = device_info->device_id;
        }

        if (device_info->device_id.empty() || device_info->host.empty() || device_info->port <= 0) {
            return false;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool is_discovery_request(const std::string& message) {
    try {
        const nlohmann::json request_json = nlohmann::json::parse(message);
        if (!request_json.is_object()) {
            return false;
        }

        if (request_json.value("type", std::string()) != kDiscoverRequestType) {
            return false;
        }
        if (request_json.value("protocol", std::string()) != kDiscoveryProtocol) {
            return false;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string build_discovery_request_message() {
    nlohmann::json request_json;
    request_json["type"] = kDiscoverRequestType;
    request_json["protocol"] = kDiscoveryProtocol;
    return request_json.dump();
}

std::string build_discovery_response_message(const DeviceInfo& local_device) {
    nlohmann::json response_json;
    response_json["type"] = kDiscoverResponseType;
    response_json["protocol"] = kDiscoveryProtocol;
    response_json["device_id"] = local_device.device_id;
    response_json["device_name"] = local_device.device_name;
    response_json["host"] = std::string();
    response_json["port"] = local_device.port;
    return response_json.dump();
}

bool receive_discovery_response(int socket_fd, DeviceInfo* device_info) {
    struct sockaddr_in source_addr;
    std::memset(&source_addr, 0, sizeof(source_addr));
    socklen_t source_addr_len = sizeof(source_addr);

    char buffer[4096];
    const ssize_t read_size = recvfrom(
        socket_fd,
        buffer,
        sizeof(buffer) - 1,
        0,
        reinterpret_cast<struct sockaddr*>(&source_addr),
        &source_addr_len);
    if (read_size < 0) {
        return false;
    }

    buffer[read_size] = '\0';

    char host_buffer[INET_ADDRSTRLEN];
    std::memset(host_buffer, 0, sizeof(host_buffer));
    const char* host_text = inet_ntop(AF_INET, &source_addr.sin_addr, host_buffer, sizeof(host_buffer));
    const std::string fallback_host = host_text == NULL ? std::string() : std::string(host_text);

    if (fallback_host.empty() || fallback_host == "0.0.0.0") {
        return false;
    }

    return parse_discovery_response_message(buffer, fallback_host, device_info);
}

void discovery_responder_loop(int socket_fd, DeviceInfo local_device) {
    const std::string response_message = build_discovery_response_message(local_device);

    while (!g_responder_stop_requested.load()) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(socket_fd, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = kDiscoveryResponderPollMs * 1000;

        const int select_result = select(socket_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (select_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (select_result == 0) {
            continue;
        }

        struct sockaddr_in source_addr;
        std::memset(&source_addr, 0, sizeof(source_addr));
        socklen_t source_addr_len = sizeof(source_addr);

        char buffer[4096];
        const ssize_t read_size = recvfrom(
            socket_fd,
            buffer,
            sizeof(buffer) - 1,
            0,
            reinterpret_cast<struct sockaddr*>(&source_addr),
            &source_addr_len);
        if (read_size < 0) {
            if (errno == EINTR) {
                continue;
            }
            continue;
        }

        buffer[read_size] = '\0';
        if (!is_discovery_request(buffer)) {
            continue;
        }

        sendto(
            socket_fd,
            response_message.data(),
            response_message.size(),
            0,
            reinterpret_cast<const struct sockaddr*>(&source_addr),
            source_addr_len);
    }
}

}  // namespace

Status list_mdns_devices(std::vector<DeviceInfo>* devices) {
    if (devices == NULL) {
        return Status(StatusCode::kDiscoveryError, "devices output cannot be null");
    }
    devices->clear();

    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        return make_io_error("Failed to create discovery socket");
    }

    int broadcast_value = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, &broadcast_value, sizeof(broadcast_value));
    int reuse_value = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_value, sizeof(reuse_value));

    struct sockaddr_in bind_addr;
    std::memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(0);
    if (bind(socket_fd, reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        const Status bind_status = make_io_error("Failed to bind discovery socket");
        close(socket_fd);
        return bind_status;
    }

    const std::string request_message = build_discovery_request_message();

    struct sockaddr_in broadcast_addr;
    std::memset(&broadcast_addr, 0, sizeof(broadcast_addr));
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(kDiscoveryPort);
    broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(
        socket_fd,
        request_message.data(),
        request_message.size(),
        0,
        reinterpret_cast<const struct sockaddr*>(&broadcast_addr),
        sizeof(broadcast_addr));

    struct sockaddr_in loopback_addr;
    std::memset(&loopback_addr, 0, sizeof(loopback_addr));
    loopback_addr.sin_family = AF_INET;
    loopback_addr.sin_port = htons(kDiscoveryPort);
    loopback_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(
        socket_fd,
        request_message.data(),
        request_message.size(),
        0,
        reinterpret_cast<const struct sockaddr*>(&loopback_addr),
        sizeof(loopback_addr));

    std::map<std::string, DeviceInfo> by_device_id;
    int elapsed_ms = 0;
    while (elapsed_ms < kDiscoveryTimeoutMs) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(socket_fd, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = kDiscoverySelectIntervalMs * 1000;

        const int select_result = select(socket_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (select_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(socket_fd);
            return make_io_error("Discovery select failed");
        }

        elapsed_ms += kDiscoverySelectIntervalMs;
        if (select_result == 0) {
            continue;
        }

        DeviceInfo device_info;
        if (receive_discovery_response(socket_fd, &device_info)) {
            by_device_id[device_info.device_id] = device_info;
        }
    }

    close(socket_fd);

    for (std::map<std::string, DeviceInfo>::const_iterator it = by_device_id.begin();
         it != by_device_id.end();
         ++it) {
        devices->push_back(it->second);
    }

    return Status::ok_status();
}

Status start_discovery_responder(const DeviceInfo& local_device) {
    if (local_device.device_id.empty()) {
        return Status(StatusCode::kDiscoveryError, "local device_id is required for discovery responder");
    }
    if (local_device.port <= 0) {
        return Status(StatusCode::kDiscoveryError, "local device port is required for discovery responder");
    }

    std::lock_guard<std::mutex> lock(g_responder_mutex);
    if (g_responder_running.load()) {
        return Status(StatusCode::kDiscoveryError, "Discovery responder is already running");
    }

    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        return make_io_error("Failed to create discovery responder socket");
    }

    int broadcast_value = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, &broadcast_value, sizeof(broadcast_value));
    int reuse_value = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_value, sizeof(reuse_value));
#ifdef SO_REUSEPORT
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEPORT, &reuse_value, sizeof(reuse_value));
#endif

    struct sockaddr_in bind_addr;
    std::memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(kDiscoveryPort);
    if (bind(socket_fd, reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        const Status bind_status = make_io_error("Failed to bind discovery responder socket");
        close(socket_fd);
        return bind_status;
    }

    g_responder_stop_requested.store(false);
    g_responder_socket_fd = socket_fd;

    try {
        g_responder_thread = std::thread(discovery_responder_loop, socket_fd, local_device);
    } catch (const std::exception& exception) {
        close(socket_fd);
        g_responder_socket_fd = -1;
        return Status(StatusCode::kDiscoveryError, exception.what());
    }

    g_responder_running.store(true);
    return Status::ok_status();
}

Status stop_discovery_responder() {
    std::lock_guard<std::mutex> lock(g_responder_mutex);
    if (!g_responder_running.load()) {
        return Status::ok_status();
    }

    g_responder_stop_requested.store(true);
    if (g_responder_thread.joinable()) {
        g_responder_thread.join();
    }

    if (g_responder_socket_fd >= 0) {
        close(g_responder_socket_fd);
        g_responder_socket_fd = -1;
    }

    g_responder_running.store(false);
    g_responder_stop_requested.store(false);
    return Status::ok_status();
}

}  // namespace internal
}  // namespace STManager
