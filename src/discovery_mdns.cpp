#include "discovery_mdns.h"

#include <cstdio>
#include <sstream>

namespace STManager {
namespace internal {
namespace {

std::vector<std::string> split_by_semicolon(const std::string& input) {
    std::vector<std::string> parts;
    std::string current_part;
    for (std::string::size_type index = 0; index < input.size(); ++index) {
        const char value = input[index];
        if (value == ';') {
            parts.push_back(current_part);
            current_part.clear();
            continue;
        }
        current_part.push_back(value);
    }
    parts.push_back(current_part);
    return parts;
}

std::string parse_txt_device_id(const std::string& txt_value) {
    const std::string key = "id=";
    std::string::size_type position = txt_value.find(key);
    if (position == std::string::npos) {
        return std::string();
    }

    position += key.size();
    std::string::size_type end_position = txt_value.find(' ', position);
    if (end_position == std::string::npos) {
        return txt_value.substr(position);
    }

    return txt_value.substr(position, end_position - position);
}

}  // namespace

Status list_mdns_devices(std::vector<DeviceInfo>* devices) {
    devices->clear();

    const char* command = "avahi-browse -rtp _stmanager-sync._tcp 2>/dev/null";
    FILE* command_pipe = popen(command, "r");
    if (command_pipe == NULL) {
        return Status(StatusCode::kDiscoveryError, "Failed to execute avahi-browse");
    }

    char line_buffer[4096];
    while (fgets(line_buffer, sizeof(line_buffer), command_pipe) != NULL) {
        const std::string line = line_buffer;
        if (line.empty() || line[0] != '=') {
            continue;
        }

        const std::vector<std::string> fields = split_by_semicolon(line);
        if (fields.size() < 10) {
            continue;
        }

        DeviceInfo device_info;
        device_info.device_name = fields[3];
        device_info.host = fields[7];

        std::istringstream port_stream(fields[8]);
        port_stream >> device_info.port;

        if (device_info.port <= 0) {
            continue;
        }

        device_info.device_id = parse_txt_device_id(fields[9]);
        if (device_info.device_id.empty()) {
            device_info.device_id = device_info.device_name;
        }

        devices->push_back(device_info);
    }

    const int exit_code = pclose(command_pipe);
    if (exit_code != 0 && devices->empty()) {
        return Status(StatusCode::kDiscoveryError, "avahi-browse did not return device results");
    }

    return Status::ok_status();
}

}  // namespace internal
}  // namespace STManager
