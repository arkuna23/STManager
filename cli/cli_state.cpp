#include "cli_state.h"

#include <STManager/data.h>

#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#else
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>

namespace STManagerCli {
namespace {

std::string join_path(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (lhs[lhs.size() - 1] == '/') {
        return lhs + rhs;
    }
    return lhs + "/" + rhs;
}

bool is_directory(const std::string& path) {
    struct stat path_stat;
    if (stat(path.c_str(), &path_stat) != 0) {
        return false;
    }
    return S_ISDIR(path_stat.st_mode);
}

bool ensure_directory_tree(const std::string& path, std::string* error_message) {
    if (path.empty()) {
        *error_message = "Directory path is empty";
        return false;
    }

    std::string current_path;
    for (std::string::size_type index = 0; index < path.size(); ++index) {
        const char value = path[index];
        current_path.push_back(value);
        if (value != '/' && index + 1 != path.size()) {
            continue;
        }

        if (current_path.empty() || current_path == "/") {
            continue;
        }

#ifdef _WIN32
        const int mkdir_result = _mkdir(current_path.c_str());
#else
        const int mkdir_result = mkdir(current_path.c_str(), 0755);
#endif
        if (mkdir_result != 0 && errno != EEXIST) {
            std::ostringstream out;
            out << "Failed to create directory " << current_path << ": " << std::strerror(errno);
            *error_message = out.str();
            return false;
        }
    }
    return true;
}

std::vector<std::string> split_segments(const std::string& path) {
    std::vector<std::string> segments;
    std::string current_segment;
    for (std::string::size_type index = 0; index < path.size(); ++index) {
        const char value = path[index];
        if (value == '/') {
            if (!current_segment.empty()) {
                segments.push_back(current_segment);
                current_segment.clear();
            }
            continue;
        }
        current_segment.push_back(value);
    }
    if (!current_segment.empty()) {
        segments.push_back(current_segment);
    }
    return segments;
}

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

std::string make_device_id() {
    std::ostringstream out;
    out << "device-"
#ifdef _WIN32
        << _getpid()
#else
        << getpid()
#endif
        << "-" << static_cast<long>(std::time(NULL));
    return out.str();
}

bool load_or_create_device_id(const std::string& device_id_path, std::string* device_id, std::string* error_message) {
    std::ifstream in_file(device_id_path.c_str());
    if (in_file.is_open()) {
        std::getline(in_file, *device_id);
        if (!device_id->empty()) {
            return true;
        }
    }

    *device_id = make_device_id();
    std::ofstream out_file(device_id_path.c_str(), std::ios::trunc);
    if (!out_file.is_open()) {
        std::ostringstream out;
        out << "Failed to write device id file: " << device_id_path;
        *error_message = out.str();
        return false;
    }
    out_file << *device_id << "\n";
    if (!out_file) {
        *error_message = "Failed writing device id";
        return false;
    }
    return true;
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

bool init_local_state(
    const std::string& root_path,
    std::string* local_device_id,
    std::string* trusted_store_path,
    std::string* error_message) {
    if (local_device_id == NULL || trusted_store_path == NULL || error_message == NULL) {
        return false;
    }
    if (!is_directory(root_path)) {
        *error_message = "Resolved root path is not a directory";
        return false;
    }

    const std::string state_dir = join_path(root_path, ".stmanager");
    if (!ensure_directory_tree(state_dir, error_message)) {
        return false;
    }

    const std::string device_id_path = join_path(state_dir, "device_id");
    if (!load_or_create_device_id(device_id_path, local_device_id, error_message)) {
        return false;
    }

    *trusted_store_path = join_path(state_dir, "trusted_devices.json");
    return true;
}

}  // namespace STManagerCli
