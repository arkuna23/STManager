#include <STManager/manager.h>

#include "cli_args.h"
#include "cli_command_selector.h"
#include "cli_pair.h"
#include "cli_state.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

void print_status_error(const STManager::Status& status) {
    std::cerr << "Error: " << status.message << "\n";
}

bool is_option_token(const std::string& token) {
    return !token.empty() && token[0] == '-';
}

bool parse_with_selected_command(
    int argc,
    char** argv,
    STManagerCli::CommandType selected_command_type,
    STManagerCli::ParsedArgs* parsed_args,
    std::string* error_message) {
    std::vector<std::string> reconstructed_args;
    reconstructed_args.reserve(static_cast<size_t>(argc) + 1);
    reconstructed_args.push_back("stmanager");
    reconstructed_args.push_back(
        selected_command_type == STManagerCli::CommandType::kRun ? "run" : "pair");

    for (int index = 1; index < argc; ++index) {
        reconstructed_args.push_back(argv[index]);
    }

    std::vector<char*> reconstructed_argv;
    reconstructed_argv.reserve(reconstructed_args.size());
    for (size_t index = 0; index < reconstructed_args.size(); ++index) {
        reconstructed_argv.push_back(const_cast<char*>(reconstructed_args[index].c_str()));
    }

    return STManagerCli::parse_cli_args(
        static_cast<int>(reconstructed_argv.size()),
        reconstructed_argv.data(),
        parsed_args,
        error_message);
}

bool parse_or_select_cli_args(
    int argc,
    char** argv,
    STManagerCli::ParsedArgs* parsed_args,
    std::string* error_message) {
    if (STManagerCli::parse_cli_args(argc, argv, parsed_args, error_message)) {
        return true;
    }

    const bool has_missing_command = argc < 2;
    const bool starts_with_option = argc >= 2 && is_option_token(argv[1]);
    if (!has_missing_command && !starts_with_option) {
        return false;
    }

    STManagerCli::CommandType selected_command_type = STManagerCli::CommandType::kUnknown;
    if (!STManagerCli::select_command(std::cin, std::cout, error_message, &selected_command_type)) {
        return false;
    }

    return parse_with_selected_command(
        argc,
        argv,
        selected_command_type,
        parsed_args,
        error_message);
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

    return STManagerCli::select_pair_device(
        pair_args,
        candidates,
        auto_selected,
        std::cin,
        std::cout,
        error_message,
        device_info);
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
    if (!parse_or_select_cli_args(argc, argv, &parsed_args, &parse_error)) {
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
