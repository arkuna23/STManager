#include "cli_state.h"

#include <STManager/data.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace STManagerCli {
namespace {

std::string dirname_path(const std::string& path) {
    if (path.empty() || path == "/") {
        return std::string();
    }
    const std::string::size_type slash_position = path.find_last_of('/');
    if (slash_position == std::string::npos) {
        return std::string();
    }
    if (slash_position == 0) {
        return "/";
    }
    return path.substr(0, slash_position);
}

bool try_resolve_root(const std::string& candidate_root, std::string* resolved_root) {
    const STManager::DataManager manager = STManager::DataManager::locate(candidate_root);
    if (!manager.is_valid()) {
        return false;
    }
    *resolved_root = manager.root_path;
    return true;
}

}  // namespace

bool detect_sillytavern_root(const std::string& explicit_root, std::string* root_path, std::string* error_message) {
    if (root_path == NULL || error_message == NULL) {
        return false;
    }

    if (!explicit_root.empty()) {
        if (try_resolve_root(explicit_root, root_path)) {
            return true;
        }
        *error_message = "Invalid --root path for SillyTavern";
        return false;
    }

    char cwd_buffer[4096];
#ifdef _WIN32
    if (_getcwd(cwd_buffer, sizeof(cwd_buffer)) == NULL) {
#else
    if (getcwd(cwd_buffer, sizeof(cwd_buffer)) == NULL) {
#endif
        *error_message = "Failed to get current directory";
        return false;
    }

    std::string current_path = cwd_buffer;
    while (!current_path.empty()) {
        if (try_resolve_root(current_path, root_path)) {
            return true;
        }

        const std::string parent_path = dirname_path(current_path);
        if (parent_path.empty() || parent_path == current_path) {
            break;
        }
        current_path = parent_path;
    }

    *error_message = "Unable to auto-detect SillyTavern root. Use --root <path>";
    return false;
}

}  // namespace STManagerCli
