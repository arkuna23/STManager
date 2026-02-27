#include "discovery_mdns.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include "platform_compat.h"

namespace STManager {
namespace internal {
namespace {

const int kDiscoveryPort = 38592;
const int kDiscoveryTimeoutMs = 2000;
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
    message_stream << prefix << ": " << socket_last_error_message();
    return Status(StatusCode::kDiscoveryError, message_stream.str());
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

void append_discovery_target(std::vector<uint32_t>* targets, uint32_t address) {
    if (targets == NULL) {
        return;
    }

    for (std::vector<uint32_t>::const_iterator it = targets->begin(); it != targets->end(); ++it) {
        if (*it == address) {
            return;
        }
    }

    targets->push_back(address);
}

void collect_discovery_targets(std::vector<uint32_t>* targets) {
    if (targets == NULL) {
        return;
    }

    targets->clear();
    append_discovery_target(targets, htonl(INADDR_BROADCAST));
    append_discovery_target(targets, htonl(INADDR_LOOPBACK));

#ifndef _WIN32
    struct ifaddrs* interface_list = NULL;
    if (getifaddrs(&interface_list) != 0 || interface_list == NULL) {
        return;
    }

    for (struct ifaddrs* it = interface_list; it != NULL; it = it->ifa_next) {
        if (it->ifa_addr == NULL || it->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((it->ifa_flags & IFF_UP) == 0) {
            continue;
        }

        if ((it->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        uint32_t target_address = 0;
        if ((it->ifa_flags & IFF_BROADCAST) != 0 &&
            it->ifa_broadaddr != NULL &&
            it->ifa_broadaddr->sa_family == AF_INET) {
            const struct sockaddr_in* broadcast_addr =
                reinterpret_cast<const struct sockaddr_in*>(it->ifa_broadaddr);
            target_address = broadcast_addr->sin_addr.s_addr;
        } else if (it->ifa_netmask != NULL && it->ifa_netmask->sa_family == AF_INET) {
            const struct sockaddr_in* interface_addr =
                reinterpret_cast<const struct sockaddr_in*>(it->ifa_addr);
            const struct sockaddr_in* netmask_addr =
                reinterpret_cast<const struct sockaddr_in*>(it->ifa_netmask);
            target_address = interface_addr->sin_addr.s_addr | ~(netmask_addr->sin_addr.s_addr);
        }

        if (target_address == 0) {
            continue;
        }
        append_discovery_target(targets, target_address);
    }

    freeifaddrs(interface_list);
#endif
}

int send_discovery_requests(int socket_fd, const std::string& request_message) {
    std::vector<uint32_t> targets;
    collect_discovery_targets(&targets);

    int successful_sends = 0;
    for (std::vector<uint32_t>::const_iterator it = targets.begin(); it != targets.end(); ++it) {
        struct sockaddr_in target_addr;
        std::memset(&target_addr, 0, sizeof(target_addr));
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(kDiscoveryPort);
        target_addr.sin_addr.s_addr = static_cast<decltype(target_addr.sin_addr.s_addr)>(*it);

        const ssize_t sent_size = sendto(
            socket_fd,
            request_message.data(),
            request_message.size(),
            0,
            reinterpret_cast<const struct sockaddr*>(&target_addr),
            sizeof(target_addr));
        if (sent_size >= 0) {
            ++successful_sends;
        }
    }

    return successful_sends;
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
            if (socket_error_is_interrupt()) {
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
            if (socket_error_is_interrupt()) {
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

    const Status runtime_status = ensure_socket_runtime();
    if (!runtime_status.ok()) {
        return runtime_status;
    }

    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        return make_io_error("Failed to create discovery socket");
    }

    int broadcast_value = 1;
    set_socket_option_int(socket_fd, SOL_SOCKET, SO_BROADCAST, broadcast_value);
    int reuse_value = 1;
    set_socket_option_int(socket_fd, SOL_SOCKET, SO_REUSEADDR, reuse_value);

    struct sockaddr_in bind_addr;
    std::memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(0);
    if (bind(socket_fd, reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        const Status bind_status = make_io_error("Failed to bind discovery socket");
        close_socket_fd(socket_fd);
        return bind_status;
    }

    const std::string request_message = build_discovery_request_message();

    const int successful_sends = send_discovery_requests(socket_fd, request_message);
    if (successful_sends == 0) {
        close_socket_fd(socket_fd);
        return Status(
            StatusCode::kDiscoveryError,
            "Failed to send discovery request on any local interface");
    }

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
            if (socket_error_is_interrupt()) {
                continue;
            }
            close_socket_fd(socket_fd);
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

    close_socket_fd(socket_fd);

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

    const Status runtime_status = ensure_socket_runtime();
    if (!runtime_status.ok()) {
        return runtime_status;
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
    set_socket_option_int(socket_fd, SOL_SOCKET, SO_BROADCAST, broadcast_value);
    int reuse_value = 1;
    set_socket_option_int(socket_fd, SOL_SOCKET, SO_REUSEADDR, reuse_value);
#ifdef SO_REUSEPORT
    set_socket_option_int(socket_fd, SOL_SOCKET, SO_REUSEPORT, reuse_value);
#endif

    struct sockaddr_in bind_addr;
    std::memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(kDiscoveryPort);
    if (bind(socket_fd, reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        const Status bind_status = make_io_error("Failed to bind discovery responder socket");
        close_socket_fd(socket_fd);
        return bind_status;
    }

    g_responder_stop_requested.store(false);
    g_responder_socket_fd = socket_fd;

    try {
        g_responder_thread = std::thread(discovery_responder_loop, socket_fd, local_device);
    } catch (const std::exception& exception) {
        close_socket_fd(socket_fd);
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
        close_socket_fd(g_responder_socket_fd);
        g_responder_socket_fd = -1;
    }

    g_responder_running.store(false);
    g_responder_stop_requested.store(false);
    return Status::ok_status();
}

}  // namespace internal
}  // namespace STManager
