#include "cli_pair.h"

#include "cli_net.h"

#include <cstdlib>
#include <istream>
#include <ostream>

namespace STManagerCli {
namespace {

bool parse_selection(const std::string& input, size_t max_count, size_t* selected_index) {
    if (input.empty()) {
        return false;
    }

    char* end_ptr = NULL;
    const long parsed_value = std::strtol(input.c_str(), &end_ptr, 10);
    if (end_ptr == input.c_str() || *end_ptr != '\0') {
        return false;
    }
    if (parsed_value <= 0 || static_cast<size_t>(parsed_value) > max_count) {
        return false;
    }

    *selected_index = static_cast<size_t>(parsed_value - 1);
    return true;
}

bool prompt_device_selection(
    const std::vector<STManager::DeviceInfo>& candidates,
    std::istream& input_stream,
    std::ostream& output_stream,
    std::string* error_message,
    STManager::DeviceInfo* selected_device) {
    if (candidates.empty()) {
        *error_message = "No sync devices found in local network";
        return false;
    }

    output_stream << "Discovered devices:\n";
    for (size_t index = 0; index < candidates.size(); ++index) {
        const STManager::DeviceInfo& device = candidates[index];
        output_stream << "  [" << (index + 1) << "] "
                      << device.device_name
                      << " (id=" << device.device_id
                      << ", " << device.host << ":" << device.port << ")\n";
    }

    output_stream << "Select device [1-" << candidates.size() << "]: ";
    std::string input;
    if (!std::getline(input_stream, input)) {
        *error_message =
            "Failed to read device selection from input. Use --device-id in non-interactive mode.";
        return false;
    }

    size_t selected_index = 0;
    if (!parse_selection(input, candidates.size(), &selected_index)) {
        *error_message = "Invalid selection. Please rerun pair and choose a valid device index.";
        return false;
    }

    *selected_device = candidates[selected_index];
    return true;
}

}  // namespace

bool select_pair_device(
    const PairRestoreArgs& pair_args,
    const std::vector<STManager::DeviceInfo>& candidates,
    const STManager::DeviceInfo& auto_selected,
    std::istream& input_stream,
    std::ostream& output_stream,
    std::string* error_message,
    STManager::DeviceInfo* selected_device) {
    if (error_message == NULL || selected_device == NULL) {
        return false;
    }

    STManager::DeviceInfo resolved_device = auto_selected;
    if (pair_args.device_id.empty()) {
        if (!prompt_device_selection(
                candidates,
                input_stream,
                output_stream,
                error_message,
                &resolved_device)) {
            return false;
        }
    }

    if (!is_connectable_host(resolved_device.host) || resolved_device.port <= 0) {
        *error_message =
            "Discovered remote endpoint is not connectable. "
            "Use --host <device_ip> and optional --port to connect directly.";
        return false;
    }

    *selected_device = resolved_device;
    return true;
}

}  // namespace STManagerCli
