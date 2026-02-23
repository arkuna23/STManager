#include "fs_ops.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <vector>

#include "path_safety.h"

namespace STManager {
namespace internal {
namespace {

const size_t kFileBufferSize = 64 * 1024;

std::string join_path(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (lhs[lhs.size() - 1] == '/') {
        return lhs + rhs;
    }
    return lhs + "/" + rhs;
}

Status make_io_error(const std::string& action, const std::string& path, int error_number) {
    std::ostringstream message;
    message << action << " failed for path " << path << ": " << std::strerror(error_number);
    return Status(StatusCode::kIoError, message.str());
}

Status copy_regular_file(const std::string& source_path, const std::string& destination_path,
                         mode_t source_mode) {
    const Status parent_status = ensure_parent_directories(destination_path, 0755);
    if (!parent_status.ok()) {
        return parent_status;
    }

    const int source_fd = open(source_path.c_str(), O_RDONLY);
    if (source_fd < 0) {
        return make_io_error("open", source_path, errno);
    }

    const int destination_fd = open(destination_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                    static_cast<int>(source_mode & 0777));
    if (destination_fd < 0) {
        const int open_error = errno;
        close(source_fd);
        return make_io_error("open", destination_path, open_error);
    }

    std::vector<char> file_buffer(kFileBufferSize);
    while (true) {
        const ssize_t read_count = read(source_fd, file_buffer.data(), file_buffer.size());
        if (read_count < 0) {
            const int read_error = errno;
            close(source_fd);
            close(destination_fd);
            return make_io_error("read", source_path, read_error);
        }
        if (read_count == 0) {
            break;
        }

        ssize_t total_written = 0;
        while (total_written < read_count) {
            const ssize_t write_count = write(destination_fd, file_buffer.data() + total_written,
                                              static_cast<size_t>(read_count - total_written));
            if (write_count < 0) {
                const int write_error = errno;
                close(source_fd);
                close(destination_fd);
                return make_io_error("write", destination_path, write_error);
            }
            total_written += write_count;
        }
    }

    close(source_fd);
    close(destination_fd);
    return Status::ok_status();
}

Status copy_path_recursive_impl(const std::string& source_path,
                                const std::string& destination_path) {
    struct stat source_stat;
    if (lstat(source_path.c_str(), &source_stat) != 0) {
        return make_io_error("lstat", source_path, errno);
    }

    if (S_ISLNK(source_stat.st_mode)) {
        return Status(StatusCode::kUnsupportedArchiveEntry,
                      "Symlink entries are not supported for recursive copy");
    }

    if (S_ISREG(source_stat.st_mode)) {
        return copy_regular_file(source_path, destination_path, source_stat.st_mode);
    }

    if (S_ISDIR(source_stat.st_mode)) {
        const Status ensure_status =
            ensure_directory_tree(destination_path, source_stat.st_mode & 0777);
        if (!ensure_status.ok()) {
            return ensure_status;
        }

        DIR* directory = opendir(source_path.c_str());
        if (directory == NULL) {
            return make_io_error("opendir", source_path, errno);
        }

        struct dirent* entry = NULL;
        while ((entry = readdir(directory)) != NULL) {
            const std::string child_name = entry->d_name;
            if (child_name == "." || child_name == "..") {
                continue;
            }

            const std::string child_source_path = join_path(source_path, child_name);
            const std::string child_destination_path = join_path(destination_path, child_name);
            const Status child_status =
                copy_path_recursive_impl(child_source_path, child_destination_path);
            if (!child_status.ok()) {
                closedir(directory);
                return child_status;
            }
        }

        closedir(directory);
        return Status::ok_status();
    }

    return Status(StatusCode::kUnsupportedArchiveEntry, "Unsupported file type for recursive copy");
}

}  // namespace

bool path_exists(const std::string& path) {
    struct stat path_stat;
    return lstat(path.c_str(), &path_stat) == 0;
}

bool path_is_directory(const std::string& path) {
    struct stat path_stat;
    if (lstat(path.c_str(), &path_stat) != 0) {
        return false;
    }
    return S_ISDIR(path_stat.st_mode);
}

Status create_temp_directory_under(const std::string& base_directory, const std::string& prefix,
                                   std::string* temp_directory) {
    if (temp_directory == NULL) {
        return Status(StatusCode::kIoError, "Temporary directory output cannot be null");
    }

    const Status ensure_status = ensure_directory_tree(base_directory, 0755);
    if (!ensure_status.ok()) {
        return ensure_status;
    }

    std::string directory_template = join_path(base_directory, prefix + "-XXXXXX");
    std::vector<char> template_buffer(directory_template.begin(), directory_template.end());
    template_buffer.push_back('\0');

    char* created_directory = mkdtemp(template_buffer.data());
    if (created_directory == NULL) {
        return make_io_error("mkdtemp", directory_template, errno);
    }

    *temp_directory = created_directory;
    return Status::ok_status();
}

Status remove_path_recursive(const std::string& path) {
    struct stat path_stat;
    if (lstat(path.c_str(), &path_stat) != 0) {
        if (errno == ENOENT) {
            return Status::ok_status();
        }
        return make_io_error("lstat", path, errno);
    }

    if (!S_ISDIR(path_stat.st_mode)) {
        if (unlink(path.c_str()) != 0) {
            return make_io_error("unlink", path, errno);
        }
        return Status::ok_status();
    }

    DIR* directory = opendir(path.c_str());
    if (directory == NULL) {
        return make_io_error("opendir", path, errno);
    }

    struct dirent* entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        const std::string child_name = entry->d_name;
        if (child_name == "." || child_name == "..") {
            continue;
        }

        const std::string child_path = join_path(path, child_name);
        const Status child_status = remove_path_recursive(child_path);
        if (!child_status.ok()) {
            closedir(directory);
            return child_status;
        }
    }

    closedir(directory);

    if (rmdir(path.c_str()) != 0) {
        return make_io_error("rmdir", path, errno);
    }

    return Status::ok_status();
}

Status copy_path_recursive(const std::string& source_path, const std::string& destination_path) {
    if (path_exists(destination_path)) {
        return Status(StatusCode::kIoError, "Destination path already exists: " + destination_path);
    }

    return copy_path_recursive_impl(source_path, destination_path);
}

Status move_or_copy_path(const std::string& source_path, const std::string& destination_path) {
    if (!path_exists(source_path)) {
        return Status(StatusCode::kIoError, "Source path does not exist: " + source_path);
    }

    if (path_exists(destination_path)) {
        return Status(StatusCode::kIoError, "Destination path already exists: " + destination_path);
    }

    const Status parent_status = ensure_parent_directories(destination_path, 0755);
    if (!parent_status.ok()) {
        return parent_status;
    }

    if (rename(source_path.c_str(), destination_path.c_str()) == 0) {
        return Status::ok_status();
    }

    const int rename_error = errno;
    if (rename_error != EXDEV) {
        std::ostringstream move_path;
        move_path << source_path << " -> " << destination_path;
        return make_io_error("rename", move_path.str(), rename_error);
    }

    const Status copy_status = copy_path_recursive(source_path, destination_path);
    if (!copy_status.ok()) {
        return copy_status;
    }

    const Status remove_status = remove_path_recursive(source_path);
    if (!remove_status.ok()) {
        remove_path_recursive(destination_path);
        return Status(StatusCode::kIoError,
                      "Cross-device move cleanup failed: " + remove_status.message);
    }

    return Status::ok_status();
}

}  // namespace internal
}  // namespace STManager
