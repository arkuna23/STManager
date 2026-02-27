#ifndef STMANAGER_CLI_ARGS_HPP
#define STMANAGER_CLI_ARGS_HPP

#include <string>

namespace STManagerCli {

const int kDefaultSyncPort = 38591;
const char* const kDefaultBackupFilePath = "st-backup.tar.zst";

enum class CommandType {
    kUnknown = 0,
    kHelp,
    kServeBackup,
    kPairRestore,
    kExportBackup,
    kRestoreBackup,
};

struct ServeBackupArgs {
    std::string root_path;
    std::string bind_host;
    int port;
    std::string pairing_code;
    std::string device_name;
    bool advertise;

    ServeBackupArgs()
        : root_path(),
          bind_host("0.0.0.0"),
          port(kDefaultSyncPort),
          pairing_code(),
          device_name(),
          advertise(true) {}
};

struct PairRestoreArgs {
    std::string root_path;
    std::string host;
    int port;
    std::string device_id;
    std::string pairing_code;
    std::string device_name;
    std::string destination_root;

    PairRestoreArgs()
        : root_path(),
          host(),
          port(0),
          device_id(),
          pairing_code(),
          device_name(),
          destination_root() {}
};

struct ExportBackupArgs {
    std::string root_path;
    std::string file_path;
    bool git_mode;

    ExportBackupArgs() : root_path(), file_path(kDefaultBackupFilePath), git_mode(false) {}
};

struct RestoreBackupArgs {
    std::string root_path;
    std::string file_path;

    RestoreBackupArgs() : root_path(), file_path(kDefaultBackupFilePath) {}
};

struct ParsedArgs {
    CommandType command_type;
    ServeBackupArgs serve_backup_args;
    PairRestoreArgs pair_restore_args;
    ExportBackupArgs export_backup_args;
    RestoreBackupArgs restore_backup_args;

    ParsedArgs()
        : command_type(CommandType::kUnknown),
          serve_backup_args(),
          pair_restore_args(),
          export_backup_args(),
          restore_backup_args() {}
};

bool parse_cli_args(int argc, char** argv, ParsedArgs* parsed_args, std::string* error_message);
std::string build_compile_time();
std::string build_help_text();

}  // namespace STManagerCli

#endif
