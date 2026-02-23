#ifndef STMANAGER_CLI_ARGS_HPP
#define STMANAGER_CLI_ARGS_HPP

#include <string>

namespace STManagerCli {

const int kDefaultSyncPort = 38591;

enum class CommandType {
    kUnknown = 0,
    kRun,
    kPair,
};

struct RunArgs {
    std::string root_path;
    std::string bind_host;
    int port;
    std::string pairing_code;
    bool advertise;

    RunArgs()
        : root_path(),
          bind_host("0.0.0.0"),
          port(kDefaultSyncPort),
          pairing_code(),
          advertise(true) {}
};

struct PairArgs {
    std::string root_path;
    std::string host;
    int port;
    std::string device_id;
    std::string pairing_code;
    std::string destination_root;
    bool git_mode;

    PairArgs() : root_path(), host(), port(0), device_id(), pairing_code(), destination_root(), git_mode(false) {}
};

struct ParsedArgs {
    CommandType command_type;
    RunArgs run_args;
    PairArgs pair_args;

    ParsedArgs() : command_type(CommandType::kUnknown), run_args(), pair_args() {}
};

bool parse_cli_args(int argc, char** argv, ParsedArgs* parsed_args, std::string* error_message);
std::string build_help_text();

}  // namespace STManagerCli

#endif
