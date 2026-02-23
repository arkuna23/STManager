#include "archive_stream.h"

#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <istream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "fs_ops.h"
#include "git_manifest.h"
#include "path_safety.h"
#include "platform_compat.h"

namespace STManager {
namespace internal {
namespace {

const char* kGitManifestPath = ".stmanager/extensions_git_manifest.json";
const size_t kStreamBufferSize = 64 * 1024;

std::string join_path(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (lhs[lhs.size() - 1] == '/') {
        return lhs + rhs;
    }
    return lhs + "/" + rhs;
}

bool is_regular_file(mode_t mode) {
    return S_ISREG(mode);
}

bool is_directory(mode_t mode) {
    return S_ISDIR(mode);
}

bool is_symlink(mode_t mode) {
    return mode_is_symlink(mode);
}

Status make_io_error(const std::string& action, const std::string& path) {
    std::ostringstream message;
    message << action << " failed for path " << path << ": " << std::strerror(errno);
    return Status(StatusCode::kIoError, message.str());
}

Status create_temp_archive_file_path(std::string* path) {
    if (path == NULL) {
        return Status(StatusCode::kIoError, "Temporary archive path output cannot be null");
    }

    char path_template[] = "/tmp/stmanager-backup-XXXXXX";
    const int temp_fd = mkstemp(path_template);
    if (temp_fd < 0) {
        return Status(StatusCode::kIoError, "Failed to create temporary backup file");
    }

    close_file_fd(temp_fd);
    *path = path_template;
    return Status::ok_status();
}

Status copy_file_to_output_stream(const std::string& file_path, std::ostream& out) {
    const int source_fd = open(file_path.c_str(), O_RDONLY);
    if (source_fd < 0) {
        return make_io_error("open", file_path);
    }

    std::vector<char> file_buffer(kStreamBufferSize);
    while (true) {
        const ssize_t read_count = read(source_fd, file_buffer.data(), file_buffer.size());
        if (read_count < 0) {
            close_file_fd(source_fd);
            return make_io_error("read", file_path);
        }
        if (read_count == 0) {
            break;
        }

        out.write(file_buffer.data(), static_cast<std::streamsize>(read_count));
        if (!out) {
            close_file_fd(source_fd);
            return Status(StatusCode::kIoError,
                          "Output stream write failed while copying backup archive");
        }
    }

    close_file_fd(source_fd);
    return Status::ok_status();
}

Status write_entry_header(struct archive* archive_writer, const std::string& archive_path,
                          const struct stat& source_stat, size_t file_size) {
    struct archive_entry* archive_entry = archive_entry_new();
    if (archive_entry == NULL) {
        return Status(StatusCode::kArchiveError, "Failed to create archive entry");
    }

    archive_entry_set_pathname(archive_entry, archive_path.c_str());
    archive_entry_set_perm(archive_entry, source_stat.st_mode & 0777);
    archive_entry_set_mtime(archive_entry, source_stat.st_mtime, 0);

    if (is_directory(source_stat.st_mode)) {
        archive_entry_set_filetype(archive_entry, AE_IFDIR);
        archive_entry_set_size(archive_entry, 0);
    } else if (is_regular_file(source_stat.st_mode)) {
        archive_entry_set_filetype(archive_entry, AE_IFREG);
        archive_entry_set_size(archive_entry, static_cast<la_int64_t>(file_size));
    } else {
        archive_entry_free(archive_entry);
        return Status(StatusCode::kUnsupportedArchiveEntry,
                      "Unsupported file type in backup source");
    }

    if (archive_write_header(archive_writer, archive_entry) != ARCHIVE_OK) {
        const std::string archive_error = archive_error_string(archive_writer);
        archive_entry_free(archive_entry);
        return Status(StatusCode::kArchiveError,
                      "Failed to write archive header: " + archive_error);
    }

    archive_entry_free(archive_entry);
    return Status::ok_status();
}

Status write_archive_data(struct archive* archive_writer, const char* data_buffer, size_t data_size,
                          const std::string& error_prefix) {
    size_t total_written = 0;
    while (total_written < data_size) {
        const la_ssize_t write_count = archive_write_data(
            archive_writer, data_buffer + total_written, data_size - total_written);
        if (write_count <= 0) {
            const std::string archive_error = archive_error_string(archive_writer);
            return Status(StatusCode::kArchiveError, error_prefix + ": " + archive_error);
        }
        total_written += static_cast<size_t>(write_count);
    }
    return Status::ok_status();
}

Status read_stream_to_memory(std::istream& in, std::string* output) {
    if (output == NULL) {
        return Status(StatusCode::kIoError, "Output buffer for stream read cannot be null");
    }

    std::string data;
    std::vector<char> buffer(kStreamBufferSize);
    while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0) {
        data.append(buffer.data(), static_cast<std::string::size_type>(in.gcount()));
    }

    if (!in.eof() && in.fail()) {
        return Status(StatusCode::kIoError, "Failed reading archive stream into memory");
    }

    *output = data;
    return Status::ok_status();
}

Status finalize_archive_entry(struct archive* archive_writer, const std::string& context) {
    const int finish_status = archive_write_finish_entry(archive_writer);
    if (finish_status != ARCHIVE_OK) {
        const std::string archive_error = archive_error_string(archive_writer);
        return Status(StatusCode::kArchiveError, context + ": " + archive_error);
    }
    return Status::ok_status();
}

Status write_regular_file_data(struct archive* archive_writer, const std::string& source_path,
                               size_t expected_file_size) {
    int source_fd = open(source_path.c_str(), O_RDONLY);
    if (source_fd < 0) {
        return make_io_error("open", source_path);
    }

    std::vector<char> file_buffer(kStreamBufferSize);
    size_t total_read_size = 0;
    while (total_read_size < expected_file_size) {
        const size_t remaining_size = expected_file_size - total_read_size;
        const size_t chunk_size =
            remaining_size < file_buffer.size() ? remaining_size : file_buffer.size();

        const ssize_t read_count = read(source_fd, file_buffer.data(), chunk_size);
        if (read_count < 0) {
            close_file_fd(source_fd);
            return make_io_error("read", source_path);
        }
        if (read_count == 0) {
            std::ostringstream message;
            message << "File changed during backup (unexpected EOF): " << source_path;
            close_file_fd(source_fd);
            return Status(StatusCode::kIoError, message.str());
        }
        total_read_size += static_cast<size_t>(read_count);

        const Status write_status =
            write_archive_data(archive_writer, file_buffer.data(), static_cast<size_t>(read_count),
                               "Failed to write file data to archive");
        if (!write_status.ok()) {
            close_file_fd(source_fd);
            return write_status;
        }
    }

    close_file_fd(source_fd);

    return finalize_archive_entry(archive_writer, "Failed to finalize archive file entry");
}

Status write_file_or_directory(struct archive* archive_writer, const std::string& source_path,
                               const std::string& archive_path);

Status write_directory_recursive(struct archive* archive_writer, const std::string& source_path,
                                 const std::string& archive_path,
                                 const std::set<std::string>* skipped_extension_names) {
    struct stat directory_stat;
    if (path_lstat(source_path.c_str(), &directory_stat) != 0) {
        return make_io_error("lstat", source_path);
    }

    const Status header_status =
        write_entry_header(archive_writer, archive_path, directory_stat, 0);
    if (!header_status.ok()) {
        return header_status;
    }
    const Status finalize_status =
        finalize_archive_entry(archive_writer, "Failed to finalize archive directory entry");
    if (!finalize_status.ok()) {
        return finalize_status;
    }

    DIR* directory = opendir(source_path.c_str());
    if (directory == NULL) {
        return make_io_error("opendir", source_path);
    }

    std::vector<std::string> child_names;
    struct dirent* child_entry = NULL;
    while ((child_entry = readdir(directory)) != NULL) {
        const std::string child_name = child_entry->d_name;
        if (child_name == "." || child_name == "..") {
            continue;
        }

        if (skipped_extension_names != NULL && archive_path == "extensions" &&
            skipped_extension_names->count(child_name) > 0) {
            continue;
        }

        child_names.push_back(child_name);
    }
    closedir(directory);

    std::sort(child_names.begin(), child_names.end());

    for (std::vector<std::string>::const_iterator it = child_names.begin(); it != child_names.end();
         ++it) {
        const std::string child_source_path = join_path(source_path, *it);
        const std::string child_archive_path = join_path(archive_path, *it);
        const Status child_status =
            write_file_or_directory(archive_writer, child_source_path, child_archive_path);
        if (!child_status.ok()) {
            return child_status;
        }
    }

    return Status::ok_status();
}

Status write_file_or_directory(struct archive* archive_writer, const std::string& source_path,
                               const std::string& archive_path) {
    struct stat source_stat;
    if (path_lstat(source_path.c_str(), &source_stat) != 0) {
        return make_io_error("lstat", source_path);
    }

    if (is_directory(source_stat.st_mode)) {
        return write_directory_recursive(archive_writer, source_path, archive_path, NULL);
    }

    if (is_symlink(source_stat.st_mode)) {
        return Status(StatusCode::kUnsupportedArchiveEntry,
                      "Symlink entries are not supported for backup");
    }

    if (is_regular_file(source_stat.st_mode)) {
        const size_t file_size = static_cast<size_t>(source_stat.st_size);
        const Status header_status =
            write_entry_header(archive_writer, archive_path, source_stat, file_size);
        if (!header_status.ok()) {
            return header_status;
        }
        return write_regular_file_data(archive_writer, source_path, file_size);
    }

    return Status(StatusCode::kUnsupportedArchiveEntry, "Unsupported file type in backup source");
}

Status write_manifest_file(struct archive* archive_writer, const std::string& json_manifest) {
    struct stat manifest_stat;
    memset(&manifest_stat, 0, sizeof(manifest_stat));
    manifest_stat.st_mode = S_IFREG | 0644;
    manifest_stat.st_mtime = time(NULL);

    const Status header_status =
        write_entry_header(archive_writer, kGitManifestPath, manifest_stat, json_manifest.size());
    if (!header_status.ok()) {
        return header_status;
    }

    if (!json_manifest.empty()) {
        const Status write_status =
            write_archive_data(archive_writer, json_manifest.c_str(), json_manifest.size(),
                               "Failed writing git manifest archive entry");
        if (!write_status.ok()) {
            return write_status;
        }
    }

    return finalize_archive_entry(archive_writer, "Failed to finalize manifest archive entry");
}

Status restore_regular_file(struct archive* archive_reader, const std::string& destination_path,
                            mode_t mode) {
    const Status parent_status = ensure_parent_directories(destination_path, 0755);
    if (!parent_status.ok()) {
        return parent_status;
    }

    int destination_fd =
        open(destination_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, static_cast<int>(mode & 0777));
    if (destination_fd < 0) {
        return make_io_error("open", destination_path);
    }

    std::vector<char> file_buffer(kStreamBufferSize);
    while (true) {
        const la_ssize_t read_count =
            archive_read_data(archive_reader, file_buffer.data(), file_buffer.size());

        if (read_count == 0) {
            break;
        }
        if (read_count < 0) {
            close_file_fd(destination_fd);
            return Status(StatusCode::kArchiveError,
                          "Failed to read archive file entry: " +
                              std::string(archive_error_string(archive_reader)));
        }

        ssize_t total_written = 0;
        while (total_written < read_count) {
            const ssize_t write_count = write(destination_fd, file_buffer.data() + total_written,
                                              static_cast<size_t>(read_count - total_written));
            if (write_count < 0) {
                close_file_fd(destination_fd);
                return make_io_error("write", destination_path);
            }
            total_written += write_count;
        }
    }

    close_file_fd(destination_fd);
    return Status::ok_status();
}

Status restore_directory(const std::string& destination_path, mode_t mode) {
    const Status directory_status = ensure_directory_tree(destination_path, mode & 0777);
    if (!directory_status.ok()) {
        return directory_status;
    }

    return Status::ok_status();
}

struct RestoreTargetPaths {
    std::string data_path;
    std::string extensions_path;
};

Status resolve_restore_target_paths(const std::string& destination_root,
                                    RestoreTargetPaths* target_paths) {
    if (target_paths == NULL) {
        return Status(StatusCode::kIoError, "Restore target output cannot be null");
    }

    target_paths->data_path = join_path(destination_root, "data");
    target_paths->extensions_path = join_path(destination_root, "public/scripts/extensions");
    return Status::ok_status();
}

Status extract_archive_to_directory(struct archive* archive_reader,
                                    const std::string& destination_root) {
    struct archive_entry* archive_entry = NULL;
    int next_header_status = ARCHIVE_OK;
    while ((next_header_status = archive_read_next_header(archive_reader, &archive_entry)) ==
           ARCHIVE_OK) {
        const char* archive_path_cstr = archive_entry_pathname(archive_entry);
        if (archive_path_cstr == NULL) {
            return Status(StatusCode::kInvalidArchiveEntry, "Archive entry has no path");
        }

        const std::string archive_path = archive_path_cstr;
        std::string destination_path;
        const Status path_status =
            join_destination_path(destination_root, archive_path, &destination_path);
        if (!path_status.ok()) {
            return path_status;
        }

        const mode_t file_type = static_cast<mode_t>(archive_entry_filetype(archive_entry));
        const mode_t entry_mode = static_cast<mode_t>(archive_entry_perm(archive_entry));
        if (file_type == AE_IFDIR) {
            const Status directory_status = restore_directory(destination_path, entry_mode);
            if (!directory_status.ok()) {
                return directory_status;
            }
            continue;
        }

        if (file_type == AE_IFREG) {
            const Status file_status =
                restore_regular_file(archive_reader, destination_path, entry_mode);
            if (!file_status.ok()) {
                return file_status;
            }
            continue;
        }

        return Status(StatusCode::kUnsupportedArchiveEntry,
                      "Restore supports only regular files and directories");
    }

    if (next_header_status != ARCHIVE_EOF) {
        const std::string archive_error = archive_error_string(archive_reader);
        return Status(StatusCode::kArchiveError, "Failed to read archive header: " + archive_error);
    }

    return Status::ok_status();
}

Status rollback_restore_targets(const RestoreTargetPaths& target_paths,
                                const std::string& backup_data_path,
                                const std::string& backup_extensions_path, bool had_existing_data,
                                bool had_existing_extensions, bool installed_data,
                                bool installed_extensions) {
    Status first_error = Status::ok_status();
    bool has_error = false;
    const auto capture_error = [&](const Status& candidate) {
        if (!candidate.ok() && !has_error) {
            first_error = candidate;
            has_error = true;
        }
    };

    if (installed_data) {
        capture_error(remove_path_recursive(target_paths.data_path));
    }
    if (installed_extensions) {
        capture_error(remove_path_recursive(target_paths.extensions_path));
    }

    if (had_existing_data && path_exists(backup_data_path)) {
        capture_error(move_or_copy_path(backup_data_path, target_paths.data_path));
    }
    if (had_existing_extensions && path_exists(backup_extensions_path)) {
        capture_error(move_or_copy_path(backup_extensions_path, target_paths.extensions_path));
    }

    return has_error ? first_error : Status::ok_status();
}

Status restore_git_manifest(const std::string& stage_root, const std::string& destination_root) {
    const std::string stage_manifest_path = join_path(stage_root, kGitManifestPath);
    const std::string destination_manifest_path = join_path(destination_root, kGitManifestPath);

    if (path_exists(stage_manifest_path) && path_is_directory(stage_manifest_path)) {
        return Status(StatusCode::kInvalidArchiveEntry,
                      "Manifest archive entry must be a regular file");
    }

    if (path_exists(stage_manifest_path)) {
        if (path_exists(destination_manifest_path)) {
            const Status remove_status = remove_path_recursive(destination_manifest_path);
            if (!remove_status.ok()) {
                return remove_status;
            }
        }

        return move_or_copy_path(stage_manifest_path, destination_manifest_path);
    }

    if (path_exists(destination_manifest_path)) {
        return remove_path_recursive(destination_manifest_path);
    }

    return Status::ok_status();
}

Status restore_from_staging_directory(const std::string& stage_root,
                                      const std::string& destination_root) {
    const std::string staged_data_path = join_path(stage_root, "data");
    if (!path_is_directory(staged_data_path)) {
        return Status(StatusCode::kInvalidArchiveEntry,
                      "Archive is missing required data directory");
    }

    const std::string staged_extensions_path = join_path(stage_root, "extensions");
    if (!path_is_directory(staged_extensions_path)) {
        return Status(StatusCode::kInvalidArchiveEntry,
                      "Archive is missing required extensions directory");
    }

    RestoreTargetPaths target_paths;
    const Status resolve_status = resolve_restore_target_paths(destination_root, &target_paths);
    if (!resolve_status.ok()) {
        return resolve_status;
    }

    const std::string backup_data_path = join_path(stage_root, ".stmanager-restore-old-data");
    const std::string backup_extensions_path =
        join_path(stage_root, ".stmanager-restore-old-extensions");
    const bool had_existing_data = path_exists(target_paths.data_path);
    const bool had_existing_extensions = path_exists(target_paths.extensions_path);

    if (had_existing_data) {
        const Status backup_data_status =
            move_or_copy_path(target_paths.data_path, backup_data_path);
        if (!backup_data_status.ok()) {
            return backup_data_status;
        }
    }

    if (had_existing_extensions) {
        const Status backup_extensions_status =
            move_or_copy_path(target_paths.extensions_path, backup_extensions_path);
        if (!backup_extensions_status.ok()) {
            const Status rollback_status =
                rollback_restore_targets(target_paths, backup_data_path, backup_extensions_path,
                                         had_existing_data, had_existing_extensions, false, false);
            if (!rollback_status.ok()) {
                return Status(StatusCode::kIoError,
                              backup_extensions_status.message +
                                  "; rollback failed: " + rollback_status.message);
            }

            return backup_extensions_status;
        }
    }

    bool installed_data = false;
    bool installed_extensions = false;

    const Status install_data_status = move_or_copy_path(staged_data_path, target_paths.data_path);
    if (!install_data_status.ok()) {
        const Status rollback_status = rollback_restore_targets(
            target_paths, backup_data_path, backup_extensions_path, had_existing_data,
            had_existing_extensions, installed_data, installed_extensions);
        if (!rollback_status.ok()) {
            return Status(
                StatusCode::kIoError,
                install_data_status.message + "; rollback failed: " + rollback_status.message);
        }

        return install_data_status;
    }
    installed_data = true;

    const Status install_extensions_status =
        move_or_copy_path(staged_extensions_path, target_paths.extensions_path);
    if (!install_extensions_status.ok()) {
        const Status rollback_status = rollback_restore_targets(
            target_paths, backup_data_path, backup_extensions_path, had_existing_data,
            had_existing_extensions, installed_data, installed_extensions);
        if (!rollback_status.ok()) {
            return Status(StatusCode::kIoError,
                          install_extensions_status.message +
                              "; rollback failed: " + rollback_status.message);
        }

        return install_extensions_status;
    }
    installed_extensions = true;

    const Status manifest_status = restore_git_manifest(stage_root, destination_root);
    if (!manifest_status.ok()) {
        const Status rollback_status = rollback_restore_targets(
            target_paths, backup_data_path, backup_extensions_path, had_existing_data,
            had_existing_extensions, installed_data, installed_extensions);
        if (!rollback_status.ok()) {
            return Status(StatusCode::kIoError, manifest_status.message + "; rollback failed: " +
                                                    rollback_status.message);
        }

        return manifest_status;
    }

    return Status::ok_status();
}

}  // namespace

Status write_backup_archive(const std::string& data_path, const std::string& extensions_path,
                            std::ostream& out, const BackupOptions& options) {
    struct archive* archive_writer = archive_write_new();
    if (archive_writer == NULL) {
        return Status(StatusCode::kArchiveError, "Failed to initialize archive writer");
    }

    archive_write_set_format_pax_restricted(archive_writer);

    if (archive_write_add_filter_zstd(archive_writer) != ARCHIVE_OK) {
        const std::string archive_error = archive_error_string(archive_writer);
        archive_write_free(archive_writer);
        return Status(StatusCode::kArchiveError, "Failed to enable zstd filter: " + archive_error);
    }

    std::string temp_archive_path;
    const Status temp_path_status = create_temp_archive_file_path(&temp_archive_path);
    if (!temp_path_status.ok()) {
        archive_write_free(archive_writer);
        return temp_path_status;
    }

    if (archive_write_open_filename(archive_writer, temp_archive_path.c_str()) != ARCHIVE_OK) {
        const std::string archive_error = archive_error_string(archive_writer);
        archive_write_free(archive_writer);
        unlink(temp_archive_path.c_str());
        return Status(StatusCode::kArchiveError, "Failed to open archive stream: " + archive_error);
    }

    std::set<std::string> skipped_extension_names;
    std::vector<GitExtensionInfo> git_extensions;
    if (options.git_mode_for_extensions) {
        const Status collect_status =
            collect_git_extension_info(extensions_path, &git_extensions, &skipped_extension_names);
        if (!collect_status.ok()) {
            archive_write_close(archive_writer);
            archive_write_free(archive_writer);
            unlink(temp_archive_path.c_str());
            return collect_status;
        }
    }

    const Status data_status = write_file_or_directory(archive_writer, data_path, "data");
    if (!data_status.ok()) {
        archive_write_close(archive_writer);
        archive_write_free(archive_writer);
        unlink(temp_archive_path.c_str());
        return data_status;
    }

    const Status extensions_root_status = write_directory_recursive(
        archive_writer, extensions_path, "extensions", &skipped_extension_names);
    if (!extensions_root_status.ok()) {
        archive_write_close(archive_writer);
        archive_write_free(archive_writer);
        unlink(temp_archive_path.c_str());
        return extensions_root_status;
    }

    if (options.git_mode_for_extensions) {
        const std::string git_manifest = build_git_manifest_json(git_extensions);
        const Status manifest_status = write_manifest_file(archive_writer, git_manifest);
        if (!manifest_status.ok()) {
            archive_write_close(archive_writer);
            archive_write_free(archive_writer);
            unlink(temp_archive_path.c_str());
            return manifest_status;
        }
    }

    if (archive_write_close(archive_writer) != ARCHIVE_OK) {
        const std::string archive_error = archive_error_string(archive_writer);
        archive_write_free(archive_writer);
        unlink(temp_archive_path.c_str());
        return Status(StatusCode::kArchiveError,
                      "Failed to close archive writer: " + archive_error);
    }

    archive_write_free(archive_writer);

    const Status copy_status = copy_file_to_output_stream(temp_archive_path, out);
    unlink(temp_archive_path.c_str());
    if (!copy_status.ok()) {
        return copy_status;
    }

    if (!out) {
        return Status(StatusCode::kIoError, "Output stream write failed");
    }

    return Status::ok_status();
}

Status validate_backup_archive(std::istream& in) {
    struct archive* archive_reader = archive_read_new();
    if (archive_reader == NULL) {
        return Status(StatusCode::kArchiveError, "Failed to initialize archive reader");
    }

    archive_read_support_format_tar(archive_reader);
    archive_read_support_filter_zstd(archive_reader);

    in.clear();
    in.seekg(0, std::ios::beg);

    std::string archive_bytes;
    const Status read_status = read_stream_to_memory(in, &archive_bytes);
    if (!read_status.ok()) {
        archive_read_free(archive_reader);
        return read_status;
    }

    if (archive_bytes.empty()) {
        archive_read_free(archive_reader);
        return Status(StatusCode::kArchiveError, "Archive stream is empty");
    }

    if (archive_read_open_memory(archive_reader, archive_bytes.data(), archive_bytes.size()) !=
        ARCHIVE_OK) {
        const std::string archive_error = archive_error_string(archive_reader);
        archive_read_free(archive_reader);
        return Status(StatusCode::kArchiveError,
                      "Failed to open archive input stream: " + archive_error);
    }

    struct archive_entry* archive_entry = NULL;
    int next_header_status = ARCHIVE_OK;
    while ((next_header_status = archive_read_next_header(archive_reader, &archive_entry)) ==
           ARCHIVE_OK) {
        const int skip_status = archive_read_data_skip(archive_reader);
        if (skip_status != ARCHIVE_OK) {
            const std::string archive_error = archive_error_string(archive_reader);
            archive_read_close(archive_reader);
            archive_read_free(archive_reader);
            return Status(StatusCode::kArchiveError, "Archive validation failed: " + archive_error);
        }
    }

    if (next_header_status != ARCHIVE_EOF) {
        const std::string archive_error = archive_error_string(archive_reader);
        archive_read_close(archive_reader);
        archive_read_free(archive_reader);
        return Status(StatusCode::kArchiveError, "Archive validation failed: " + archive_error);
    }

    if (archive_read_close(archive_reader) != ARCHIVE_OK) {
        const std::string archive_error = archive_error_string(archive_reader);
        archive_read_free(archive_reader);
        return Status(StatusCode::kArchiveError,
                      "Failed to close archive reader: " + archive_error);
    }

    archive_read_free(archive_reader);
    in.clear();
    in.seekg(0, std::ios::beg);
    return Status::ok_status();
}

Status restore_backup_archive(std::istream& in, const std::string& destination_root) {
    if (destination_root.empty()) {
        return Status(StatusCode::kInvalidRoot, "Restore destination root cannot be empty");
    }

    const Status ensure_root_status = ensure_directory_tree(destination_root, 0755);
    if (!ensure_root_status.ok()) {
        return ensure_root_status;
    }

    struct archive* archive_reader = archive_read_new();
    if (archive_reader == NULL) {
        return Status(StatusCode::kArchiveError, "Failed to initialize archive reader");
    }

    archive_read_support_format_tar(archive_reader);
    archive_read_support_filter_zstd(archive_reader);

    std::string archive_bytes;
    const Status read_status = read_stream_to_memory(in, &archive_bytes);
    if (!read_status.ok()) {
        archive_read_free(archive_reader);
        return read_status;
    }

    if (archive_bytes.empty()) {
        archive_read_free(archive_reader);
        return Status(StatusCode::kArchiveError, "Archive stream is empty");
    }

    if (archive_read_open_memory(archive_reader, archive_bytes.data(), archive_bytes.size()) !=
        ARCHIVE_OK) {
        const std::string archive_error = archive_error_string(archive_reader);
        archive_read_free(archive_reader);
        return Status(StatusCode::kArchiveError,
                      "Failed to open archive input stream: " + archive_error);
    }

    std::string stage_root;
    const Status stage_status =
        create_temp_directory_under(destination_root, ".stmanager-restore-stage", &stage_root);
    if (!stage_status.ok()) {
        archive_read_close(archive_reader);
        archive_read_free(archive_reader);
        return stage_status;
    }

    const Status extract_status = extract_archive_to_directory(archive_reader, stage_root);
    if (!extract_status.ok()) {
        archive_read_close(archive_reader);
        archive_read_free(archive_reader);
        remove_path_recursive(stage_root);
        return extract_status;
    }

    if (archive_read_close(archive_reader) != ARCHIVE_OK) {
        const std::string archive_error = archive_error_string(archive_reader);
        archive_read_free(archive_reader);
        remove_path_recursive(stage_root);
        return Status(StatusCode::kArchiveError,
                      "Failed to close archive reader: " + archive_error);
    }

    archive_read_free(archive_reader);

    const Status restore_status = restore_from_staging_directory(stage_root, destination_root);
    const Status cleanup_status = remove_path_recursive(stage_root);
    if (!restore_status.ok()) {
        return restore_status;
    }
    if (!cleanup_status.ok()) {
        return cleanup_status;
    }

    return Status::ok_status();
}

}  // namespace internal
}  // namespace STManager
