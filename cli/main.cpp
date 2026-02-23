#include <STManager/manager.h>

#include "cli_args.h"
#include "cli_net.h"
#include "cli_state.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void print_status_error(const STManager::Status& status) {
    std::cerr << "Error: " << status.message << "\n";
}

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
    const std::vector<STManager::DeviceInfo>& discovered_devices,
    STManager::DeviceInfo* selected_device,
    std::string* error_message) {
    if (discovered_devices.empty()) {
        *error_message = "No sync devices found in local network";
        return false;
    }

    std::cout << "Discovered devices:\n";
    for (size_t index = 0; index < discovered_devices.size(); ++index) {
        const STManager::DeviceInfo& device = discovered_devices[index];
        std::cout << "  [" << (index + 1) << "] "
                  << device.device_name
                  << " (id=" << device.device_id
                  << ", " << device.host << ":" << device.port << ")\n";
    }

    std::cout << "Select device [1-" << discovered_devices.size() << "]: ";
    std::string input;
    if (!std::getline(std::cin, input)) {
        *error_message =
            "Failed to read device selection from input. Use --device-id in non-interactive mode.";
        return false;
    }

    size_t selected_index = 0;
    if (!parse_selection(input, discovered_devices.size(), &selected_index)) {
        *error_message = "Invalid selection. Please rerun pair and choose a valid device index.";
        return false;
    }

    *selected_device = discovered_devices[selected_index];
    return true;
}

bool resolve_remote_device(
    const STManagerCli::PairArgs& pair_args,
    const STManager::Manager& manager,
    STManager::DeviceInfo* device_info,
    std::string* error_message) {
    STManager::PairSyncRequest pair_sync_request;
    pair_sync_request.device_id = pair_args.device_id;
    pair_sync_request.host = pair_args.host;
    pair_sync_request.port = pair_args.port;

    std::vector<STManager::DeviceInfo> candidates;
    STManager::DeviceInfo auto_selected;
    const STManager::Status resolve_status = manager.resolve_pair_target(
        pair_sync_request,
        &candidates,
        &auto_selected);
    if (!resolve_status.ok()) {
        *error_message = resolve_status.message;
        return false;
    }

    STManager::DeviceInfo resolved_device;
    if (pair_args.device_id.empty() && candidates.size() > 1) {
        if (!prompt_device_selection(candidates, &resolved_device, error_message)) {
            return false;
        }
    } else {
        resolved_device = auto_selected;
    }

    if (!STManagerCli::is_connectable_host(resolved_device.host) || resolved_device.port <= 0) {
        *error_message =
            "Discovered remote endpoint is not connectable. "
            "Use --host <device_ip> and optional --port to connect directly.";
        return false;
    }

    *device_info = resolved_device;
    return true;
}

int run_command(const STManagerCli::RunArgs& args) {
    std::string root_path;
    std::string error_message;
    if (!STManagerCli::detect_sillytavern_root(args.root_path, &root_path, &error_message)) {
        std::cerr << "Error: " << error_message << "\n";
        return 1;
    }

    STManager::Manager manager;
    const STManager::Status create_status = STManager::Manager::create_from_root(root_path, &manager);
    if (!create_status.ok()) {
        print_status_error(create_status);
        return 1;
    }

    STManager::RunSyncOptions run_sync_options;
    run_sync_options.server_options.bind_host = args.bind_host;
    run_sync_options.server_options.port = args.port;
    run_sync_options.server_options.pairing_code = args.pairing_code;
    run_sync_options.server_options.advertise = args.advertise;
    run_sync_options.server_options.advertise_name = manager.local_device_id();

    std::cout << "Starting sync server on " << run_sync_options.server_options.bind_host
              << ":" << run_sync_options.server_options.port
              << " (device_id=" << manager.local_device_id() << ")\n";

    STManager::RunSyncResult run_sync_result;
    const STManager::Status run_status = manager.run_sync(run_sync_options, &run_sync_result);
    if (!run_status.ok()) {
        print_status_error(run_status);
        return 1;
    }

    std::cout << "Server stopped.\n";
    std::cout << "Root: " << root_path << "\n";
    std::cout << "Device ID: " << manager.local_device_id() << "\n";
    std::cout << "Bound port: " << run_sync_result.bound_port << "\n";
    return 0;
}

int pair_command(const STManagerCli::PairArgs& args) {
    std::string root_path;
    std::string error_message;
    if (!STManagerCli::detect_sillytavern_root(args.root_path, &root_path, &error_message)) {
        std::cerr << "Error: " << error_message << "\n";
        return 1;
    }

    STManager::Manager manager;
    const STManager::Status create_status = STManager::Manager::create_from_root(root_path, &manager);
    if (!create_status.ok()) {
        print_status_error(create_status);
        return 1;
    }

    STManager::DeviceInfo remote_device;
    if (!resolve_remote_device(args, manager, &remote_device, &error_message)) {
        std::cerr << "Error: " << error_message << "\n";
        return 1;
    }

    STManager::PairSyncOptions pair_sync_options;
    pair_sync_options.pairing_options.pairing_code = args.pairing_code;
    pair_sync_options.pairing_options.remember_device = true;
    pair_sync_options.sync_options.destination_root_override = args.destination_root;
    pair_sync_options.sync_options.backup_options.git_mode_for_extensions = args.git_mode;

    STManager::PairSyncResult pair_sync_result;
    const STManager::Status pair_sync_status = manager.pair_sync(
        remote_device,
        pair_sync_options,
        &pair_sync_result);
    if (!pair_sync_status.ok()) {
        print_status_error(pair_sync_status);
        return 1;
    }

    std::cout << "Pull completed from device " << pair_sync_result.selected_device.device_id << ".\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    STManagerCli::ParsedArgs parsed_args;
    std::string parse_error;
    if (!STManagerCli::parse_cli_args(argc, argv, &parsed_args, &parse_error)) {
        std::cerr << "Error: " << parse_error << "\n\n" << STManagerCli::build_help_text() << "\n";
        return 1;
    }

    if (parsed_args.command_type == STManagerCli::CommandType::kRun) {
        return run_command(parsed_args.run_args);
    }
    if (parsed_args.command_type == STManagerCli::CommandType::kPair) {
        return pair_command(parsed_args.pair_args);
    }

    std::cerr << STManagerCli::build_help_text() << "\n";
    return 1;
}
