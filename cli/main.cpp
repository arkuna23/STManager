#include <STManager/manager.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on
#endif

#include "cli_args.h"
#include "cli_command_selector.h"
#include "cli_net.h"
#include "cli_pair.h"
#include "cli_state.h"

namespace {

void print_status_error(const STManager::Status& status) {
    std::cerr << "Error: " << status.message << "\n";
}

std::atomic<bool> g_interrupt_requested(false);
std::atomic<const char*> g_last_stage("startup");

void set_last_stage(const char* stage) {
    g_last_stage.store(stage, std::memory_order_relaxed);
}

const char* current_last_stage() {
    const char* stage = g_last_stage.load(std::memory_order_relaxed);
    return stage == NULL ? "unknown" : stage;
}

std::string current_time_string() {
    const std::time_t now = std::time(NULL);
    struct tm local_tm;
    std::memset(&local_tm, 0, sizeof(local_tm));
#ifdef _WIN32
    if (localtime_s(&local_tm, &now) != 0) {
        return "unknown-time";
    }
#else
    if (localtime_r(&now, &local_tm) == NULL) {
        return "unknown-time";
    }
#endif
    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local_tm) == 0) {
        return "unknown-time";
    }
    return buffer;
}

void append_crash_log(const std::string& line) {
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cerr << line << "\n";
    std::ofstream log_file("stmanager-crash.log", std::ios::app);
    if (!log_file.is_open()) {
        return;
    }
    log_file << line << "\n";
    log_file.flush();
}

std::string build_crash_line(const std::string& source, const std::string& detail) {
    std::ostringstream message_stream;
    message_stream << "Fatal: stage=process.crash." << source << "; time=" << current_time_string()
                   << "; build_time=" << STManagerCli::build_compile_time()
                   << "; last_stage=" << current_last_stage();
    if (!detail.empty()) {
        message_stream << "; " << detail;
    }
    return message_stream.str();
}

#ifdef _WIN32
LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* exception_pointers) {
    DWORD exception_code = 0;
    if (exception_pointers != NULL && exception_pointers->ExceptionRecord != NULL) {
        exception_code = exception_pointers->ExceptionRecord->ExceptionCode;
    }
    std::ostringstream detail_stream;
    detail_stream << "exception_code=0x" << std::hex << exception_code;
    append_crash_log(build_crash_line("seh", detail_stream.str()));
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void crash_signal_handler(int signal_number) {
    std::ostringstream detail_stream;
    detail_stream << "signal=" << signal_number;
    append_crash_log(build_crash_line("signal", detail_stream.str()));
    std::_Exit(128 + signal_number);
}

void terminate_handler() {
    std::string terminate_detail = "detail=unknown";
    try {
        std::exception_ptr current = std::current_exception();
        if (current) {
            try {
                std::rethrow_exception(current);
            } catch (const std::exception& exception) {
                terminate_detail = std::string("detail=") + exception.what();
            } catch (...) {
                terminate_detail = "detail=non-std exception";
            }
        }
    } catch (...) {
        terminate_detail = "detail=failed reading current_exception";
    }

    append_crash_log(build_crash_line("terminate", terminate_detail));
    std::_Exit(134);
}

class CrashGuardScope {
public:
    CrashGuardScope()
        : previous_abrt_(SIG_ERR),
          previous_segv_(SIG_ERR),
          previous_terminate_(NULL)
#ifdef _WIN32
          ,
          previous_unhandled_filter_(NULL)
#endif
    {
        previous_terminate_ = std::set_terminate(terminate_handler);
        previous_abrt_ = std::signal(SIGABRT, crash_signal_handler);
        previous_segv_ = std::signal(SIGSEGV, crash_signal_handler);
#ifdef _WIN32
        previous_unhandled_filter_ = SetUnhandledExceptionFilter(unhandled_exception_filter);
#endif
    }

    ~CrashGuardScope() {
        if (previous_abrt_ != SIG_ERR) {
            std::signal(SIGABRT, previous_abrt_);
        }
        if (previous_segv_ != SIG_ERR) {
            std::signal(SIGSEGV, previous_segv_);
        }
        if (previous_terminate_ != NULL) {
            std::set_terminate(previous_terminate_);
        }
#ifdef _WIN32
        SetUnhandledExceptionFilter(previous_unhandled_filter_);
#endif
    }

private:
    typedef void (*SignalHandler)(int);
    SignalHandler previous_abrt_;
    SignalHandler previous_segv_;
    std::terminate_handler previous_terminate_;
#ifdef _WIN32
    LPTOP_LEVEL_EXCEPTION_FILTER previous_unhandled_filter_;
#endif
};

void handle_sigint(int) {
    g_interrupt_requested.store(true);
}

class SignalScope {
public:
    SignalScope() : previous_handler_(SIG_ERR), has_previous_(false) {
        g_interrupt_requested.store(false);
        previous_handler_ = std::signal(SIGINT, handle_sigint);
        has_previous_ = previous_handler_ != SIG_ERR;
    }

    ~SignalScope() {
        if (has_previous_) {
            std::signal(SIGINT, previous_handler_);
        }
    }

private:
    typedef void (*SignalHandler)(int);
    SignalHandler previous_handler_;
    bool has_previous_;
};

#ifdef _WIN32
bool utf16_to_utf8(const std::wstring& utf16_text, std::string* utf8_text) {
    if (utf8_text == NULL) {
        return false;
    }
    utf8_text->clear();

    if (utf16_text.empty()) {
        return true;
    }

    DWORD conversion_flags = 0;
#ifdef WC_ERR_INVALID_CHARS
    conversion_flags = WC_ERR_INVALID_CHARS;
#endif

    const int utf8_length =
        WideCharToMultiByte(CP_UTF8, conversion_flags, utf16_text.c_str(), -1, NULL, 0, NULL, NULL);
    if (utf8_length <= 0) {
        return false;
    }

    std::string utf8_buffer(static_cast<size_t>(utf8_length), '\0');
    const int convert_result = WideCharToMultiByte(CP_UTF8, conversion_flags, utf16_text.c_str(),
                                                   -1, &utf8_buffer[0], utf8_length, NULL, NULL);
    if (convert_result <= 0) {
        return false;
    }

    if (!utf8_buffer.empty() && utf8_buffer[utf8_buffer.size() - 1] == '\0') {
        utf8_buffer.resize(utf8_buffer.size() - 1);
    }

    *utf8_text = utf8_buffer;
    return true;
}

bool build_utf8_argv_from_windows_command_line(std::vector<std::string>* utf8_args,
                                               std::vector<char*>* utf8_argv,
                                               std::string* error_message) {
    if (utf8_args == NULL || utf8_argv == NULL || error_message == NULL) {
        return false;
    }

    utf8_args->clear();
    utf8_argv->clear();

    LPWSTR command_line = GetCommandLineW();
    if (command_line == NULL) {
        *error_message = "Failed reading command line";
        return false;
    }

    int wide_argc = 0;
    LPWSTR* wide_argv = CommandLineToArgvW(command_line, &wide_argc);
    if (wide_argv == NULL || wide_argc <= 0) {
        std::ostringstream message_stream;
        message_stream << "CommandLineToArgvW failed: "
                       << static_cast<unsigned long>(GetLastError());
        *error_message = message_stream.str();
        return false;
    }

    utf8_args->reserve(static_cast<size_t>(wide_argc));
    for (int index = 0; index < wide_argc; ++index) {
        std::string utf8_arg;
        const std::wstring wide_arg = wide_argv[index] == NULL ? L"" : wide_argv[index];
        if (!utf16_to_utf8(wide_arg, &utf8_arg)) {
            LocalFree(wide_argv);
            std::ostringstream message_stream;
            message_stream << "Failed converting argv[" << index << "] to UTF-8";
            *error_message = message_stream.str();
            return false;
        }
        utf8_args->push_back(utf8_arg);
    }
    LocalFree(wide_argv);

    utf8_argv->reserve(utf8_args->size());
    for (size_t index = 0; index < utf8_args->size(); ++index) {
        utf8_argv->push_back(const_cast<char*>((*utf8_args)[index].c_str()));
    }

    return !utf8_argv->empty();
}
#endif

bool is_option_token(const char* token) {
    return token != NULL && token[0] == '-';
}

bool is_help_option_token(const char* token) {
    if (token == NULL) {
        return false;
    }
    const std::string token_string = token;
    return token_string == "--help" || token_string == "-h" || token_string == "help";
}

bool action_to_tokens(STManagerCli::CommandType command_type, std::string* command_token,
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

bool parse_with_selected_action(int argc, char** argv, STManagerCli::CommandType selected_action,
                                STManagerCli::ParsedArgs* parsed_args, std::string* error_message) {
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

    return STManagerCli::parse_cli_args(static_cast<int>(reconstructed_argv.size()),
                                        reconstructed_argv.data(), parsed_args, error_message);
}

bool parse_or_select_cli_args(int argc, char** argv, STManagerCli::ParsedArgs* parsed_args,
                              std::string* error_message) {
    if (argc >= 2 && is_help_option_token(argv[1])) {
        return STManagerCli::parse_cli_args(argc, argv, parsed_args, error_message);
    }

    if (argc >= 2 && !is_option_token(argv[1])) {
        return STManagerCli::parse_cli_args(argc, argv, parsed_args, error_message);
    }

    STManagerCli::CommandType selected_action = STManagerCli::CommandType::kUnknown;
    if (!STManagerCli::select_action(std::cin, std::cout, error_message, &selected_action)) {
        return false;
    }

    return parse_with_selected_action(argc, argv, selected_action, parsed_args, error_message);
}

bool resolve_remote_device(const STManagerCli::PairRestoreArgs& pair_args,
                           const STManager::Manager& manager, STManager::DeviceInfo* device_info,
                           std::string* error_message) {
    STManager::PairSyncRequest pair_sync_request;
    pair_sync_request.device_id = pair_args.device_id;
    pair_sync_request.host = pair_args.host;
    pair_sync_request.port = pair_args.port;

    std::vector<STManager::DeviceInfo> candidates;
    STManager::DeviceInfo auto_selected;
    const STManager::Status resolve_status =
        manager.resolve_pair_target(pair_sync_request, &candidates, &auto_selected);
    if (!resolve_status.ok()) {
        *error_message = resolve_status.message;
        return false;
    }

    return STManagerCli::select_pair_device(pair_args, candidates, auto_selected, std::cin,
                                            std::cout, error_message, device_info);
}

int serve_backup_command(const STManagerCli::ServeBackupArgs& args) {
    set_last_stage("serve.command.start");

    std::string root_path;
    std::string error_message;
    if (!STManagerCli::detect_sillytavern_root(args.root_path, &root_path, &error_message)) {
        std::cerr << "Error: " << error_message << "\n";
        return 1;
    }

    STManager::Manager manager;
    const STManager::Status create_status =
        STManager::Manager::create_from_root(root_path, &manager);
    if (!create_status.ok()) {
        print_status_error(create_status);
        return 1;
    }

    STManager::ServeSyncOptions serve_sync_options;
    serve_sync_options.server_options.bind_host = args.bind_host;
    serve_sync_options.server_options.port = args.port;
    serve_sync_options.server_options.pairing_code = args.pairing_code;
    serve_sync_options.server_options.advertise = args.advertise;
    serve_sync_options.device_name = args.device_name;

    std::cout << "Starting sync server on " << serve_sync_options.server_options.bind_host << ":"
              << serve_sync_options.server_options.port
              << " (device_id=" << manager.local_device_id() << ")\n";

    std::unique_ptr<STManager::SyncTaskHandle> serve_handle =
        manager.serve_sync(serve_sync_options);
    if (!serve_handle.get()) {
        std::cerr << "Error: failed creating serve sync task handle\n";
        return 1;
    }

    SignalScope signal_scope;
    bool printed_runtime_info = false;
    bool stop_requested_by_user = false;
    STManager::DeviceInfo latest_info = serve_handle->info();
    while (serve_handle->is_running()) {
        set_last_stage("serve.loop.poll_state");
        latest_info = serve_handle->info();
        const bool endpoint_ready = latest_info.port > 0;
        if (!printed_runtime_info && endpoint_ready) {
            const std::string runtime_host = STManagerCli::runtime_display_host(
                latest_info.host, serve_sync_options.server_options.bind_host);
            std::cout << "Server running on " << runtime_host << ":" << latest_info.port
                      << " (device_id=" << latest_info.device_id
                      << ", bind_host=" << serve_sync_options.server_options.bind_host << ")\n";
            printed_runtime_info = true;
        }

        if (g_interrupt_requested.load()) {
            set_last_stage("serve.loop.user_stop");
            serve_handle->stop();
            g_interrupt_requested.store(false);
            stop_requested_by_user = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    set_last_stage("serve.loop.wait_result");
    const STManager::Status serve_status = serve_handle->wait();
    if (!serve_status.ok()) {
        print_status_error(serve_status);
        return 1;
    }
    if (!stop_requested_by_user) {
        std::ostringstream message_stream;
        message_stream << "stage=serve.cli.unexpected_exit";
        if (printed_runtime_info) {
            message_stream << "; server_exited_after_accept=true";
        }
        if (latest_info.port > 0) {
            message_stream << "; last_endpoint="
                           << STManagerCli::runtime_display_host(
                                  latest_info.host, serve_sync_options.server_options.bind_host)
                           << ":" << latest_info.port;
        }

        print_status_error(
            STManager::Status(STManager::StatusCode::kSyncProtocolError, message_stream.str()));
        return 1;
    }

    latest_info = serve_handle->info();
    std::cout << "Server stopped.\n";
    std::cout << "Root: " << root_path << "\n";
    std::cout << "Device ID: " << latest_info.device_id << "\n";
    std::cout << "Device name: " << latest_info.device_name << "\n";
    std::cout << "Bound host: "
              << STManagerCli::runtime_display_host(latest_info.host,
                                                    serve_sync_options.server_options.bind_host)
              << "\n";
    std::cout << "Bound port: " << latest_info.port << "\n";
    return 0;
}

int pair_restore_command(const STManagerCli::PairRestoreArgs& args) {
    set_last_stage("pair.command.start");

    std::string root_path;
    std::string error_message;
    if (!STManagerCli::detect_sillytavern_root(args.root_path, &root_path, &error_message)) {
        std::cerr << "Error: " << error_message << "\n";
        return 1;
    }

    STManager::Manager manager;
    const STManager::Status create_status =
        STManager::Manager::create_from_root(root_path, &manager);
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
    pair_sync_options.device_name = args.device_name;
    pair_sync_options.sync_options.destination_root_override = args.destination_root;

    STManager::PairSyncResult pair_sync_result;
    const STManager::Status pair_sync_status =
        manager.pair_sync(remote_device, pair_sync_options, &pair_sync_result);
    if (!pair_sync_status.ok()) {
        print_status_error(pair_sync_status);
        return 1;
    }

    std::cout << "Restore completed from device " << pair_sync_result.selected_device.device_id
              << ".\n";
    return 0;
}

int export_backup_command(const STManagerCli::ExportBackupArgs& args) {
    set_last_stage("export.command.start");

    std::string root_path;
    std::string error_message;
    if (!STManagerCli::detect_sillytavern_root(args.root_path, &root_path, &error_message)) {
        std::cerr << "Error: " << error_message << "\n";
        return 1;
    }

    STManager::Manager manager;
    const STManager::Status create_status =
        STManager::Manager::create_from_root(root_path, &manager);
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

    std::cout << "Backup exported: " << export_result.file_path << " ("
              << export_result.bytes_written << " bytes)\n";
    return 0;
}

int restore_backup_command(const STManagerCli::RestoreBackupArgs& args) {
    set_last_stage("restore.command.start");

    std::string root_path;
    std::string error_message;
    if (!STManagerCli::detect_sillytavern_root(args.root_path, &root_path, &error_message)) {
        std::cerr << "Error: " << error_message << "\n";
        return 1;
    }

    STManager::Manager manager;
    const STManager::Status create_status =
        STManager::Manager::create_from_root(root_path, &manager);
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
    CrashGuardScope crash_guard_scope;
    set_last_stage("main.parse_args");

    int parse_argc = argc;
    char** parse_argv = argv;
#ifdef _WIN32
    std::vector<std::string> utf8_args;
    std::vector<char*> utf8_argv;
    std::string argv_error;
    if (!build_utf8_argv_from_windows_command_line(&utf8_args, &utf8_argv, &argv_error)) {
        std::cerr << "Build time: " << STManagerCli::build_compile_time() << "\n";
        std::cerr << "Error: " << argv_error << "\n";
        return 1;
    }
    parse_argc = static_cast<int>(utf8_argv.size());
    parse_argv = utf8_argv.data();
#endif

    STManagerCli::ParsedArgs parsed_args;
    std::string parse_error;
    if (!parse_or_select_cli_args(parse_argc, parse_argv, &parsed_args, &parse_error)) {
        std::cerr << "Build time: " << STManagerCli::build_compile_time() << "\n";
        std::cerr << "Error: " << parse_error << "\n\n" << STManagerCli::build_help_text() << "\n";
        return 1;
    }

    std::cout << "Build time: " << STManagerCli::build_compile_time() << "\n";

    if (parsed_args.command_type == STManagerCli::CommandType::kServeBackup) {
        set_last_stage("main.dispatch.serve");
        return serve_backup_command(parsed_args.serve_backup_args);
    }
    if (parsed_args.command_type == STManagerCli::CommandType::kHelp) {
        set_last_stage("main.dispatch.help");
        std::cout << STManagerCli::build_help_text() << "\n";
        return 0;
    }
    if (parsed_args.command_type == STManagerCli::CommandType::kPairRestore) {
        set_last_stage("main.dispatch.pair");
        return pair_restore_command(parsed_args.pair_restore_args);
    }
    if (parsed_args.command_type == STManagerCli::CommandType::kExportBackup) {
        set_last_stage("main.dispatch.export");
        return export_backup_command(parsed_args.export_backup_args);
    }
    if (parsed_args.command_type == STManagerCli::CommandType::kRestoreBackup) {
        set_last_stage("main.dispatch.restore");
        return restore_backup_command(parsed_args.restore_backup_args);
    }

    std::cerr << STManagerCli::build_help_text() << "\n";
    return 1;
}
