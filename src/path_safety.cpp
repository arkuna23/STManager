#include "path_safety.h"

#include <sys/stat.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <vector>

namespace STManager {
namespace internal {
namespace {

bool path_exists(const std::string& path, struct stat* path_stat) {
    return lstat(path.c_str(), path_stat) == 0;
}

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> path_parts;
    std::string current_part;
    for (std::string::size_type i = 0; i < path.size(); ++i) {
        const char value = path[i];
        if (value == '/') {
            path_parts.push_back(current_part);
            current_part.clear();
            continue;
        }
        current_part.push_back(value);
    }
    path_parts.push_back(current_part);
    return path_parts;
}

std::string join_path(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (lhs[lhs.size() - 1] == '/') {
        return lhs + rhs;
    }
    return lhs + "/" + rhs;
}

Status validate_no_symlink_or_nondir(const std::string& directory_path) {
    struct stat path_stat;
    if (!path_exists(directory_path, &path_stat)) {
        return Status(StatusCode::kIoError, "Directory path component does not exist");
    }
    if (S_ISLNK(path_stat.st_mode)) {
        return Status(StatusCode::kInvalidArchiveEntry, "Refusing to traverse symlink in destination path");
    }
    if (!S_ISDIR(path_stat.st_mode)) {
        return Status(StatusCode::kInvalidArchiveEntry, "Destination path component is not a directory");
    }
    return Status::ok_status();
}

}  // namespace

Status validate_archive_relative_path(const std::string& archive_path) {
    if (archive_path.empty()) {
        return Status(StatusCode::kInvalidArchiveEntry, "Archive entry path is empty");
    }

    if (archive_path[0] == '/') {
        return Status(StatusCode::kInvalidArchiveEntry, "Absolute archive path is not allowed");
    }

    const std::vector<std::string> path_parts = split_path(archive_path);
    for (std::vector<std::string>::size_type index = 0; index < path_parts.size(); ++index) {
        const std::string& path_part = path_parts[index];
        const bool is_last_part = (index + 1 == path_parts.size());
        if (path_part.empty() && is_last_part) {
            continue;
        }
        if (path_part.empty() || path_part == "." || path_part == "..") {
            return Status(StatusCode::kInvalidArchiveEntry, "Archive path contains invalid segment");
        }
    }

    return Status::ok_status();
}

Status join_destination_path(
    const std::string& destination_root,
    const std::string& archive_path,
    std::string* destination_path) {
    const Status path_status = validate_archive_relative_path(archive_path);
    if (!path_status.ok()) {
        return path_status;
    }

    *destination_path = join_path(destination_root, archive_path);
    return Status::ok_status();
}

Status ensure_directory_tree(const std::string& directory_path, int mode) {
    if (directory_path.empty()) {
        return Status(StatusCode::kIoError, "Directory path is empty");
    }

    std::string current_path;
    const bool is_absolute = directory_path[0] == '/';
    if (is_absolute) {
        current_path = "/";
    }

    const std::vector<std::string> path_parts = split_path(directory_path);
    for (std::vector<std::string>::const_iterator it = path_parts.begin(); it != path_parts.end(); ++it) {
        const std::string& path_part = *it;
        if (path_part.empty()) {
            continue;
        }

        current_path = join_path(current_path, path_part);

        struct stat path_stat;
        if (!path_exists(current_path, &path_stat)) {
            if (mkdir(current_path.c_str(), static_cast<mode_t>(mode)) != 0) {
                std::ostringstream message;
                message << "Failed to create directory: " << current_path << ", reason: "
                        << std::strerror(errno);
                return Status(StatusCode::kIoError, message.str());
            }
            continue;
        }

        const Status validate_status = validate_no_symlink_or_nondir(current_path);
        if (!validate_status.ok()) {
            return validate_status;
        }
    }

    return Status::ok_status();
}

Status ensure_parent_directories(const std::string& file_path, int mode) {
    const std::string::size_type separator_index = file_path.find_last_of('/');
    if (separator_index == std::string::npos) {
        return Status::ok_status();
    }

    const std::string parent_directory = file_path.substr(0, separator_index);
    if (parent_directory.empty()) {
        return Status::ok_status();
    }

    return ensure_directory_tree(parent_directory, mode);
}

}  // namespace internal
}  // namespace STManager
