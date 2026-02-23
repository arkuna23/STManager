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

bool is_option_token(const char* token) {
    return token != NULL && token[0] == '-';
}

bool action_to_tokens(
    STManagerCli::CommandType command_type,
    std::string* command_token,
    std::string* action_token) {
    if (command_token == NULL || action_token == NULL) {
        return false;
    }

    if (command_type == STManagerCli::CommandType::kServeBackup) {
        *command_token = "serve";
        *action_token = "backup";
        return true;
    }
    if (command_type == STManagerCli::CommandType::kPairRestore) {
        *command_token = "pair";
        *action_token = "restore";
        return true;
    }
    if (command_type == STManagerCli::CommandType::kExportBackup) {
        *command_token = "export";
        *action_token = "backup";
        return true;
    }
    if (command_type == STManagerCli::CommandType::kRestoreBackup) {
        *command_token = "restore";
        *action_token = "backup";
        return true;
    }

    return false;
}

bool parse_with_selected_action(
    int argc,
    char** argv,
    STManagerCli::CommandType selected_action,
    STManagerCli::ParsedArgs* parsed_args,
    std::string* error_message) {
    std::string command_token;
    std::string action_token;
    if (!action_to_tokens(selected_action, &command_token, &action_token)) {
        *error_message = "Unknown selected action";
        return false;
    }

    std::vector<std::string> reconstructed_args;
    reconstructed_args.reserve(static_cast<size_t>(argc) + 2);
    reconstructed_args.push_back("stmanager");
    reconstructed_args.push_back(command_token);
    reconstructed_args.push_back(action_token);

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
    if (argc >= 2 && !is_option_token(argv[1])) {
        return STManagerCli::parse_cli_args(argc, argv, parsed_args, error_message);
    }

    STManagerCli::CommandType selected_action = STManagerCli::CommandType::kUnknown;
    if (!STManagerCli::select_action(std::cin, std::cout, error_message, &selected_action)) {
        return false;
    }

    return parse_with_selected_action(argc, argv, selected_action, parsed_args, error_message);
}

bool resolve_remote_device(
    const STManagerCli::PairRestoreArgs& pair_args,
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

int serve_backup_command(const STManagerCli::ServeBackupArgs& args) {
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

    STManager::ServeSyncOptions serve_sync_options;
    serve_sync_options.server_options.bind_host = args.bind_host;
    serve_sync_options.server_options.port = args.port;
    serve_sync_options.server_options.pairing_code = args.pairing_code;
    serve_sync_options.server_options.advertise = args.advertise;
    serve_sync_options.server_options.advertise_name = manager.local_device_id();

    std::cout << "Starting sync server on " << serve_sync_options.server_options.bind_host
              << ":" << serve_sync_options.server_options.port
              << " (device_id=" << manager.local_device_id() << ")\n";

    STManager::ServeSyncResult serve_sync_result;
    const STManager::Status serve_status =
        manager.serve_sync(serve_sync_options, &serve_sync_result);
    if (!serve_status.ok()) {
        print_status_error(serve_status);
        return 1;
    }

    std::cout << "Server stopped.\n";
    std::cout << "Root: " << root_path << "\n";
    std::cout << "Device ID: " << manager.local_device_id() << "\n";
    std::cout << "Bound port: " << serve_sync_result.bound_port << "\n";
    return 0;
}

int pair_restore_command(const STManagerCli::PairRestoreArgs& args) {
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

    STManager::PairSyncResult pair_sync_result;
    const STManager::Status pair_sync_status = manager.pair_sync(
        remote_device,
        pair_sync_options,
        &pair_sync_result);
    if (!pair_sync_status.ok()) {
        print_status_error(pair_sync_status);
        return 1;
    }

    std::cout << "Restore completed from device " << pair_sync_result.selected_device.device_id
              << ".\n";
    return 0;
}

int export_backup_command(const STManagerCli::ExportBackupArgs& args) {
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

    STManager::ExportBackupOptions export_options;
    export_options.file_path = args.file_path;
    export_options.backup_options.git_mode_for_extensions = args.git_mode;

    STManager::ExportBackupResult export_result;
    const STManager::Status export_status = manager.export_backup(export_options, &export_result);
    if (!export_status.ok()) {
        print_status_error(export_status);
        return 1;
    }

    std::cout << "Backup exported: " << export_result.file_path
              << " (" << export_result.bytes_written << " bytes)\n";
    return 0;
}

int restore_backup_command(const STManagerCli::RestoreBackupArgs& args) {
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

    STManager::RestoreBackupOptions restore_options;
    restore_options.file_path = args.file_path;

    const STManager::Status restore_status = manager.restore_backup(restore_options);
    if (!restore_status.ok()) {
        print_status_error(restore_status);
        return 1;
    }

    std::cout << "Backup restored from: " << restore_options.file_path << "\n";
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

    if (parsed_args.command_type == STManagerCli::CommandType::kServeBackup) {
        return serve_backup_command(parsed_args.serve_backup_args);
    }
    if (parsed_args.command_type == STManagerCli::CommandType::kPairRestore) {
        return pair_restore_command(parsed_args.pair_restore_args);
    }
    if (parsed_args.command_type == STManagerCli::CommandType::kExportBackup) {
        return export_backup_command(parsed_args.export_backup_args);
    }
    if (parsed_args.command_type == STManagerCli::CommandType::kRestoreBackup) {
        return restore_backup_command(parsed_args.restore_backup_args);
    }

    std::cerr << STManagerCli::build_help_text() << "\n";
    return 1;
}
