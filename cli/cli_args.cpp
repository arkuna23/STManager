#include "cli_args.h"

#include <cstdlib>
#include <sstream>

namespace STManagerCli {
namespace {

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

bool parse_run_args(int argc, char** argv, ParsedArgs* parsed_args, std::string* error_message) {
    RunArgs args;

    for (int index = 2; index < argc; ++index) {
        const std::string flag = argv[index];
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
        if (flag == "--advertise" && index + 1 < argc) {
            if (!parse_bool_value(argv[++index], &args.advertise)) {
                *error_message = "Invalid --advertise value. Use true or false";
                return false;
            }
            continue;
        }

        *error_message = "Unknown run argument: " + flag;
        return false;
    }

    parsed_args->command_type = CommandType::kRun;
    parsed_args->run_args = args;
    return true;
}

bool parse_pair_args(int argc, char** argv, ParsedArgs* parsed_args, std::string* error_message) {
    PairArgs args;

    for (int index = 2; index < argc; ++index) {
        const std::string flag = argv[index];
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
        if (flag == "--dest-root" && index + 1 < argc) {
            args.destination_root = argv[++index];
            continue;
        }
        if (flag == "--git-mode") {
            args.git_mode = true;
            continue;
        }

        *error_message = "Unknown pair argument: " + flag;
        return false;
    }

    parsed_args->command_type = CommandType::kPair;
    parsed_args->pair_args = args;
    return true;
}

}  // namespace

bool parse_cli_args(int argc, char** argv, ParsedArgs* parsed_args, std::string* error_message) {
    if (argc < 2) {
        *error_message = "Missing command";
        return false;
    }
    if (parsed_args == NULL || error_message == NULL) {
        return false;
    }

    const std::string command = argv[1];
    if (command == "run") {
        return parse_run_args(argc, argv, parsed_args, error_message);
    }
    if (command == "pair") {
        return parse_pair_args(argc, argv, parsed_args, error_message);
    }

    *error_message = "Unknown command: " + command;
    return false;
}

std::string build_help_text() {
    std::ostringstream out;
    out << "STManager CLI\n\n";
    out << "Commands:\n";
    out << "  stmanager run [--root <path>] [--bind <host>] [--port <port>] "
           "[--pairing-code <code>] [--advertise true|false]\n";
    out << "  stmanager pair [--root <path>] [--host <ip>] [--port <port>] [--device-id <id>] "
           "[--pairing-code <code>] [--dest-root <path>] [--git-mode]\n";
    out << "\nDefaults:\n";
    out << "  run --port defaults to " << kDefaultSyncPort << "\n";
    out << "  pair with --host but no --port uses " << kDefaultSyncPort << "\n";
    return out.str();
}

}  // namespace STManagerCli
