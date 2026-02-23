#ifndef STMANAGER_CLI_COMMAND_SELECTOR_HPP
#define STMANAGER_CLI_COMMAND_SELECTOR_HPP

#include "cli_args.h"

#include <iosfwd>
#include <string>

namespace STManagerCli {

bool select_command(
    std::istream& input_stream,
    std::ostream& output_stream,
    std::string* error_message,
    CommandType* command_type);

}  // namespace STManagerCli

#endif
