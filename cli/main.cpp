#include <STManager/data.h>
#include <STManager/sync.h>
#include <STManager/tcp_transport.h>

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
    STManager::SyncManager* sync_manager,
    STManager::DeviceInfo* device_info,
    std::string* error_message) {
    STManager::DeviceInfo resolved_device;
    resolved_device.device_id = pair_args.device_id;
    resolved_device.device_name = pair_args.device_id;
    resolved_device.host = pair_args.host;
    resolved_device.port = pair_args.port;

    if (pair_args.device_id.empty()) {
        std::vector<STManager::DeviceInfo> discovered_devices;
        const STManager::Status discover_status = sync_manager->discover_devices(&discovered_devices);
        if (!discover_status.ok()) {
            *error_message =
                "Unable to discover device automatically: " + discover_status.message +
                ". Use --device-id/--host/--port to bypass auto-discovery.";
            return false;
        }

        std::vector<STManager::DeviceInfo> filtered_devices;
        for (std::vector<STManager::DeviceInfo>::const_iterator it = discovered_devices.begin();
             it != discovered_devices.end();
             ++it) {
            if (!pair_args.host.empty() && pair_args.host != it->host) {
                continue;
            }
            filtered_devices.push_back(*it);
        }

        if (filtered_devices.empty()) {
            *error_message = "No discovered device matched current pair arguments";
            return false;
        }

        if (!prompt_device_selection(filtered_devices, &resolved_device, error_message)) {
            return false;
        }
        if (pair_args.port > 0) {
            resolved_device.port = pair_args.port;
        }
        *device_info = resolved_device;
        return true;
    }

    if (!resolved_device.host.empty() && resolved_device.port <= 0) {
        resolved_device.port = STManagerCli::kDefaultSyncPort;
    }

    if (resolved_device.host.empty() && resolved_device.port > 0) {
        *error_message = "--host is required when --port is provided";
        return false;
    }

    if (resolved_device.host.empty()) {
        std::vector<STManager::DeviceInfo> discovered_devices;
        const STManager::Status discover_status = sync_manager->discover_devices(&discovered_devices);
        if (!discover_status.ok()) {
            *error_message =
                "Unable to discover device automatically: " + discover_status.message +
                ". Use --host and optional --port to bypass auto-discovery.";
            return false;
        }

        bool found = false;
        for (std::vector<STManager::DeviceInfo>::const_iterator it = discovered_devices.begin();
             it != discovered_devices.end();
             ++it) {
            if (it->device_id != pair_args.device_id) {
                continue;
            }
            found = true;
            if (resolved_device.host.empty()) {
                resolved_device.host = it->host;
            }
            if (resolved_device.port <= 0) {
                resolved_device.port = it->port;
            }
            break;
        }

        if (!found) {
            *error_message = "Device not found via discovery. Set --host/--port explicitly.";
            return false;
        }
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

    std::string local_device_id;
    std::string trusted_store_path;
    if (!STManagerCli::init_local_state(root_path, &local_device_id, &trusted_store_path, &error_message)) {
        std::cerr << "Error: " << error_message << "\n";
        return 1;
    }

    const STManager::DataManager data_manager = STManager::DataManager::locate(root_path);
    if (!data_manager.is_valid()) {
        print_status_error(data_manager.last_status());
        return 1;
    }

    STManager::JsonTrustedDeviceStore trusted_store(trusted_store_path);
    STManager::ServerOptions server_options;
    server_options.bind_host = args.bind_host;
    server_options.port = args.port;
    server_options.pairing_code = args.pairing_code;
    server_options.advertise = args.advertise;
    server_options.advertise_name = local_device_id;

    std::cout << "Starting sync server on " << server_options.bind_host
              << ":" << server_options.port
              << " (device_id=" << local_device_id << ")\n";

    int bound_port = 0;
    const STManager::Status run_status = STManager::run_sync_server(
        data_manager,
        local_device_id,
        &trusted_store,
        server_options,
        &bound_port);
    if (!run_status.ok()) {
        print_status_error(run_status);
        return 1;
    }

    std::cout << "Server stopped.\n";
    std::cout << "Root: " << root_path << "\n";
    std::cout << "Device ID: " << local_device_id << "\n";
    std::cout << "Bound port: " << bound_port << "\n";
    return 0;
}

int pair_command(const STManagerCli::PairArgs& args) {
    std::string root_path;
    std::string error_message;
    if (!STManagerCli::detect_sillytavern_root(args.root_path, &root_path, &error_message)) {
        std::cerr << "Error: " << error_message << "\n";
        return 1;
    }

    std::string local_device_id;
    std::string trusted_store_path;
    if (!STManagerCli::init_local_state(root_path, &local_device_id, &trusted_store_path, &error_message)) {
        std::cerr << "Error: " << error_message << "\n";
        return 1;
    }

    const STManager::DataManager data_manager = STManager::DataManager::locate(root_path);
    if (!data_manager.is_valid()) {
        print_status_error(data_manager.last_status());
        return 1;
    }

    STManager::TcpSyncTransport transport;
    STManager::MdnsDeviceDiscovery discovery;
    STManager::JsonTrustedDeviceStore trusted_store(trusted_store_path);
    STManager::SyncManager sync_manager(
        data_manager,
        local_device_id,
        &transport,
        &discovery,
        &trusted_store);

    STManager::DeviceInfo remote_device;
    if (!resolve_remote_device(args, &sync_manager, &remote_device, &error_message)) {
        std::cerr << "Error: " << error_message << "\n";
        return 1;
    }

    const STManager::Status load_status = trusted_store.load();
    if (!load_status.ok()) {
        print_status_error(load_status);
        return 1;
    }

    if (!trusted_store.is_trusted(remote_device.device_id)) {
        STManager::PairingOptions pairing_options;
        pairing_options.pairing_code = args.pairing_code;
        pairing_options.remember_device = true;
        const STManager::Status pair_status = sync_manager.pair_device(remote_device, pairing_options);
        if (!pair_status.ok()) {
            print_status_error(pair_status);
            return 1;
        }
    }

    STManager::SyncOptions sync_options;
    sync_options.destination_root_override = args.destination_root;
    sync_options.backup_options.git_mode_for_extensions = args.git_mode;

    const STManager::Status pull_status = sync_manager.pull_from_device(remote_device, sync_options);
    if (!pull_status.ok()) {
        print_status_error(pull_status);
        return 1;
    }

    std::cout << "Pull completed from device " << remote_device.device_id << ".\n";
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
