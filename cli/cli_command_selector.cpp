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
    if (parsed_value < 1 || parsed_value > 4) {
        return false;
    }

    *selected_value = static_cast<int>(parsed_value);
    return true;
}

}  // namespace

bool select_action(
    std::istream& input_stream,
    std::ostream& output_stream,
    std::string* error_message,
    CommandType* command_type) {
    if (error_message == NULL || command_type == NULL) {
        return false;
    }

    output_stream << "Select action:\n";
    output_stream << "  [1] serve backup\n";
    output_stream << "  [2] pair restore\n";
    output_stream << "  [3] export backup\n";
    output_stream << "  [4] restore backup\n";
    output_stream << "Select action [1-4]: ";

    std::string input;
    if (!std::getline(input_stream, input)) {
        *error_message = "Failed to read action selection from input.";
        return false;
    }

    int selected_value = 0;
    if (!parse_selection(input, &selected_value)) {
        *error_message = "Invalid action selection. Please enter 1 to 4.";
        return false;
    }

    if (selected_value == 1) {
        *command_type = CommandType::kServeBackup;
    } else if (selected_value == 2) {
        *command_type = CommandType::kPairRestore;
    } else if (selected_value == 3) {
        *command_type = CommandType::kExportBackup;
    } else {
        *command_type = CommandType::kRestoreBackup;
    }

    return true;
}

}  // namespace STManagerCli
