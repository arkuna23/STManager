#include "cli_command_selector.h"

#include <cstdlib>
#include <istream>
#include <ostream>

namespace STManagerCli {
namespace {

bool parse_selection(const std::string& input, int* selected_value) {
    if (input.empty()) {
        return false;
    }

    char* end_ptr = NULL;
    const long parsed_value = std::strtol(input.c_str(), &end_ptr, 10);
    if (end_ptr == input.c_str() || *end_ptr != '\0') {
        return false;
    }
    if (parsed_value < 1 || parsed_value > 2) {
        return false;
    }

    *selected_value = static_cast<int>(parsed_value);
    return true;
}

}  // namespace

bool select_command(
    std::istream& input_stream,
    std::ostream& output_stream,
    std::string* error_message,
    CommandType* command_type) {
    if (error_message == NULL || command_type == NULL) {
        return false;
    }

    output_stream << "Select command:\n";
    output_stream << "  [1] run\n";
    output_stream << "  [2] pair\n";
    output_stream << "Enter selection [1-2]: ";

    std::string input;
    if (!std::getline(input_stream, input)) {
        *error_message = "Failed to read command selection from input.";
        return false;
    }

    int selected_value = 0;
    if (!parse_selection(input, &selected_value)) {
        *error_message = "Invalid command selection. Please enter 1 or 2.";
        return false;
    }

    *command_type = (selected_value == 1) ? CommandType::kRun : CommandType::kPair;
    return true;
}

}  // namespace STManagerCli
