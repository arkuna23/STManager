#include "cli_args.h"

#include <cstdlib>
#include <sstream>

namespace STManagerCli {
namespace {

bool is_help_token(const std::string& token) {
    return token == "--help" || token == "-h" || token == "help";
}

bool parse_int_value(const std::string& value, int* output) {
    if (value.empty()) {
        return false;
    }

    char* end_ptr = NULL;
    const long parsed_value = std::strtol(value.c_str(), &end_ptr, 10);
    if (end_ptr == value.c_str() || *end_ptr != '\0') {
        return false;
    }
    if (parsed_value <= 0 || parsed_value > 65535) {
        return false;
    }

    *output = static_cast<int>(parsed_value);
    return true;
}

bool parse_bool_value(const std::string& value, bool* output) {
    if (value == "true" || value == "1" || value == "yes") {
        *output = true;
        return true;
    }
    if (value == "false" || value == "0" || value == "no") {
        *output = false;
        return true;
    }
    return false;
}

bool parse_serve_backup_args(
    int argc,
    char** argv,
    ParsedArgs* parsed_args,
    std::string* error_message) {
    ServeBackupArgs args;

    for (int index = 3; index < argc; ++index) {
        const std::string flag = argv[index];
        if (flag == "--help" || flag == "-h") {
            parsed_args->command_type = CommandType::kHelp;
            return true;
        }
        if (flag == "--root" && index + 1 < argc) {
            args.root_path = argv[++index];
            continue;
        }
        if (flag == "--bind" && index + 1 < argc) {
            args.bind_host = argv[++index];
            continue;
        }
        if (flag == "--port" && index + 1 < argc) {
            if (!parse_int_value(argv[++index], &args.port)) {
                *error_message = "Invalid --port value";
                return false;
            }
            continue;
        }
        if (flag == "--pairing-code" && index + 1 < argc) {
            args.pairing_code = argv[++index];
            continue;
        }
        if (flag == "--device-name" && index + 1 < argc) {
            args.device_name = argv[++index];
            continue;
        }
        if (flag == "--advertise" && index + 1 < argc) {
            if (!parse_bool_value(argv[++index], &args.advertise)) {
                *error_message = "Invalid --advertise value. Use true or false";
                return false;
            }
            continue;
        }

        *error_message = "Unknown serve backup argument: " + flag;
        return false;
    }

    parsed_args->command_type = CommandType::kServeBackup;
    parsed_args->serve_backup_args = args;
    return true;
}

bool parse_pair_restore_args(
    int argc,
    char** argv,
    ParsedArgs* parsed_args,
    std::string* error_message) {
    PairRestoreArgs args;

    for (int index = 3; index < argc; ++index) {
        const std::string flag = argv[index];
        if (flag == "--help" || flag == "-h") {
            parsed_args->command_type = CommandType::kHelp;
            return true;
        }
        if (flag == "--root" && index + 1 < argc) {
            args.root_path = argv[++index];
            continue;
        }
        if (flag == "--host" && index + 1 < argc) {
            args.host = argv[++index];
            continue;
        }
        if (flag == "--port" && index + 1 < argc) {
            if (!parse_int_value(argv[++index], &args.port)) {
                *error_message = "Invalid --port value";
                return false;
            }
            continue;
        }
        if (flag == "--device-id" && index + 1 < argc) {
            args.device_id = argv[++index];
            continue;
        }
        if (flag == "--pairing-code" && index + 1 < argc) {
            args.pairing_code = argv[++index];
            continue;
        }
        if (flag == "--device-name" && index + 1 < argc) {
            args.device_name = argv[++index];
            continue;
        }
        if (flag == "--dest-root" && index + 1 < argc) {
            args.destination_root = argv[++index];
            continue;
        }

        *error_message = "Unknown pair restore argument: " + flag;
        return false;
    }

    parsed_args->command_type = CommandType::kPairRestore;
    parsed_args->pair_restore_args = args;
    return true;
}

bool parse_export_backup_args(
    int argc,
    char** argv,
    ParsedArgs* parsed_args,
    std::string* error_message) {
    ExportBackupArgs args;

    for (int index = 3; index < argc; ++index) {
        const std::string flag = argv[index];
        if (flag == "--help" || flag == "-h") {
            parsed_args->command_type = CommandType::kHelp;
            return true;
        }
        if (flag == "--root" && index + 1 < argc) {
            args.root_path = argv[++index];
            continue;
        }
        if (flag == "--file" && index + 1 < argc) {
            args.file_path = argv[++index];
            continue;
        }
        if (flag == "--git-mode") {
            args.git_mode = true;
            continue;
        }

        *error_message = "Unknown export backup argument: " + flag;
        return false;
    }

    parsed_args->command_type = CommandType::kExportBackup;
    parsed_args->export_backup_args = args;
    return true;
}

bool parse_restore_backup_args(
    int argc,
    char** argv,
    ParsedArgs* parsed_args,
    std::string* error_message) {
    RestoreBackupArgs args;

    for (int index = 3; index < argc; ++index) {
        const std::string flag = argv[index];
        if (flag == "--help" || flag == "-h") {
            parsed_args->command_type = CommandType::kHelp;
            return true;
        }
        if (flag == "--root" && index + 1 < argc) {
            args.root_path = argv[++index];
            continue;
        }
        if (flag == "--file" && index + 1 < argc) {
            args.file_path = argv[++index];
            continue;
        }

        *error_message = "Unknown restore backup argument: " + flag;
        return false;
    }

    parsed_args->command_type = CommandType::kRestoreBackup;
    parsed_args->restore_backup_args = args;
    return true;
}

}  // namespace

bool parse_cli_args(int argc, char** argv, ParsedArgs* parsed_args, std::string* error_message) {
    if (parsed_args == NULL || error_message == NULL) {
        return false;
    }
    if (argc < 2) {
        *error_message = "Missing command";
        return false;
    }

    const std::string command = argv[1];
    if (is_help_token(command)) {
        parsed_args->command_type = CommandType::kHelp;
        return true;
    }
    if (command != "serve" && command != "pair" && command != "export" && command != "restore") {
        *error_message = "Unknown command: " + command;
        return false;
    }

    if (argc < 3) {
        *error_message = "Missing action";
        return false;
    }
    const std::string action = argv[2];

    if (command == "serve") {
        if (action != "backup") {
            *error_message = "Unknown serve action: " + action;
            return false;
        }
        return parse_serve_backup_args(argc, argv, parsed_args, error_message);
    }

    if (command == "pair") {
        if (action != "restore") {
            *error_message = "Unknown pair action: " + action;
            return false;
        }
        return parse_pair_restore_args(argc, argv, parsed_args, error_message);
    }

    if (command == "export") {
        if (action != "backup") {
            *error_message = "Unknown export action: " + action;
            return false;
        }
        return parse_export_backup_args(argc, argv, parsed_args, error_message);
    }

    if (command == "restore") {
        if (action != "backup") {
            *error_message = "Unknown restore action: " + action;
            return false;
        }
        return parse_restore_backup_args(argc, argv, parsed_args, error_message);
    }

    *error_message = "Unknown command: " + command;
    return false;
}

std::string build_compile_time() {
#if defined(__DATE__) && defined(__TIME__)
    return std::string(__DATE__) + " " + std::string(__TIME__);
#else
    return "unknown";
#endif
}

std::string build_help_text() {
    std::ostringstream out;
    out << "STManager CLI\n\n";
    out << "  stmanager --help | -h\n\n";
    out << "Commands:\n";
    out << "  stmanager serve backup [--root <path>] [--bind <host>] [--port <port>] "
           "[--pairing-code <code>] [--device-name <name>] [--advertise true|false]\n";
    out << "  stmanager pair restore [--root <path>] [--host <ip>] [--port <port>] "
           "[--device-id <id>] [--pairing-code <code>] [--device-name <name>] "
           "[--dest-root <path>]\n";
    out << "  stmanager export backup [--root <path>] [--file <path>] [--git-mode]\n";
    out << "  stmanager restore backup [--root <path>] [--file <path>]\n";
    out << "\nDefaults:\n";
    out << "  serve backup --port defaults to " << kDefaultSyncPort << "\n";
    out << "  pair restore with --host but no --port uses " << kDefaultSyncPort << "\n";
    out << "  export/restore backup --file defaults to " << kDefaultBackupFilePath << "\n";
    out << "\nTip:\n";
    out << "  Run `stmanager` with no arguments to open interactive action menu.\n";
    return out.str();
}

}  // namespace STManagerCli
