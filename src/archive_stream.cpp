#include "archive_stream.h"

#include "git_manifest.h"
#include "path_safety.h"

#include <archive.h>
#include <archive_entry.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <istream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace STManager {
namespace internal {
namespace {

const char* kGitManifestPath = ".stmanager/extensions_git_manifest.json";
const size_t kStreamBufferSize = 64 * 1024;

struct OstreamWriteContext {
    std::ostream* out;
};

struct IstreamReadContext {
    std::istream* in;
    std::vector<char> buffer;
};

std::string join_path(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (lhs[lhs.size() - 1] == '/') {
        return lhs + rhs;
    }
    return lhs + "/" + rhs;
}

bool is_regular_file(mode_t mode) { return S_ISREG(mode); }

bool is_directory(mode_t mode) { return S_ISDIR(mode); }

bool is_symlink(mode_t mode) { return S_ISLNK(mode); }

Status make_io_error(const std::string& action, const std::string& path) {
    std::ostringstream message;
    message << action << " failed for path " << path << ": " << std::strerror(errno);
    return Status(StatusCode::kIoError, message.str());
}

int archive_write_open_callback(struct archive*, void*) { return ARCHIVE_OK; }

la_ssize_t archive_write_callback(struct archive*, void* client_data, const void* buffer, size_t length) {
    OstreamWriteContext* context = static_cast<OstreamWriteContext*>(client_data);
    context->out->write(static_cast<const char*>(buffer), static_cast<std::streamsize>(length));
    if (!(*context->out)) {
        return -1;
    }
    return static_cast<la_ssize_t>(length);
}

int archive_write_close_callback(struct archive*, void*) { return ARCHIVE_OK; }

int archive_read_open_callback(struct archive*, void*) { return ARCHIVE_OK; }

la_ssize_t archive_read_callback(struct archive*, void* client_data, const void** buffer) {
    IstreamReadContext* context = static_cast<IstreamReadContext*>(client_data);
    context->in->read(context->buffer.data(), static_cast<std::streamsize>(context->buffer.size()));
    const std::streamsize read_count = context->in->gcount();
    if (read_count <= 0) {
        return 0;
    }

    *buffer = context->buffer.data();
    return static_cast<la_ssize_t>(read_count);
}

int archive_read_close_callback(struct archive*, void*) { return ARCHIVE_OK; }

Status write_entry_header(
    struct archive* archive_writer,
    const std::string& archive_path,
    const struct stat& source_stat,
    size_t file_size) {
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
        return Status(StatusCode::kUnsupportedArchiveEntry, "Unsupported file type in backup source");
    }

    if (archive_write_header(archive_writer, archive_entry) != ARCHIVE_OK) {
        const std::string archive_error = archive_error_string(archive_writer);
        archive_entry_free(archive_entry);
        return Status(StatusCode::kArchiveError, "Failed to write archive header: " + archive_error);
    }

    archive_entry_free(archive_entry);
    return Status::ok_status();
}

Status write_archive_data(
    struct archive* archive_writer,
    const char* data_buffer,
    size_t data_size,
    const std::string& error_prefix) {
    size_t total_written = 0;
    while (total_written < data_size) {
        const la_ssize_t write_count = archive_write_data(
            archive_writer,
            data_buffer + total_written,
            data_size - total_written);
        if (write_count <= 0) {
            const std::string archive_error = archive_error_string(archive_writer);
            return Status(StatusCode::kArchiveError, error_prefix + ": " + archive_error);
        }
        total_written += static_cast<size_t>(write_count);
    }
    return Status::ok_status();
}

Status write_regular_file_data(struct archive* archive_writer, const std::string& source_path) {
    int source_fd = open(source_path.c_str(), O_RDONLY);
    if (source_fd < 0) {
        return make_io_error("open", source_path);
    }

    std::vector<char> file_buffer(kStreamBufferSize);
    while (true) {
        const ssize_t read_count = read(source_fd, file_buffer.data(), file_buffer.size());
        if (read_count < 0) {
            close(source_fd);
            return make_io_error("read", source_path);
        }
        if (read_count == 0) {
            break;
        }

        const Status write_status = write_archive_data(
            archive_writer,
            file_buffer.data(),
            static_cast<size_t>(read_count),
            "Failed to write file data to archive");
        if (!write_status.ok()) {
            close(source_fd);
            return write_status;
        }
    }

    close(source_fd);
    return Status::ok_status();
}

Status write_file_or_directory(
    struct archive* archive_writer,
    const std::string& source_path,
    const std::string& archive_path);

Status write_directory_recursive(
    struct archive* archive_writer,
    const std::string& source_path,
    const std::string& archive_path,
    const std::set<std::string>* skipped_extension_names) {
    struct stat directory_stat;
    if (lstat(source_path.c_str(), &directory_stat) != 0) {
        return make_io_error("lstat", source_path);
    }

    const Status header_status = write_entry_header(archive_writer, archive_path, directory_stat, 0);
    if (!header_status.ok()) {
        return header_status;
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

    for (std::vector<std::string>::const_iterator it = child_names.begin(); it != child_names.end(); ++it) {
        const std::string child_source_path = join_path(source_path, *it);
        const std::string child_archive_path = join_path(archive_path, *it);
        const Status child_status = write_file_or_directory(
            archive_writer, child_source_path, child_archive_path);
        if (!child_status.ok()) {
            return child_status;
        }
    }

    return Status::ok_status();
}

Status write_file_or_directory(
    struct archive* archive_writer,
    const std::string& source_path,
    const std::string& archive_path) {
    struct stat source_stat;
    if (lstat(source_path.c_str(), &source_stat) != 0) {
        return make_io_error("lstat", source_path);
    }

    if (is_directory(source_stat.st_mode)) {
        return write_directory_recursive(archive_writer, source_path, archive_path, NULL);
    }

    if (is_symlink(source_stat.st_mode)) {
        return Status(
            StatusCode::kUnsupportedArchiveEntry,
            "Symlink entries are not supported for backup");
    }

    if (is_regular_file(source_stat.st_mode)) {
        const Status header_status = write_entry_header(
            archive_writer, archive_path, source_stat, static_cast<size_t>(source_stat.st_size));
        if (!header_status.ok()) {
            return header_status;
        }
        return write_regular_file_data(archive_writer, source_path);
    }

    return Status(StatusCode::kUnsupportedArchiveEntry, "Unsupported file type in backup source");
}

Status write_manifest_file(struct archive* archive_writer, const std::string& json_manifest) {
    struct stat manifest_stat;
    memset(&manifest_stat, 0, sizeof(manifest_stat));
    manifest_stat.st_mode = S_IFREG | 0644;
    manifest_stat.st_mtime = time(NULL);

    const Status header_status = write_entry_header(
        archive_writer, kGitManifestPath, manifest_stat, json_manifest.size());
    if (!header_status.ok()) {
        return header_status;
    }

    if (!json_manifest.empty()) {
        const Status write_status = write_archive_data(
            archive_writer,
            json_manifest.c_str(),
            json_manifest.size(),
            "Failed writing git manifest archive entry");
        if (!write_status.ok()) {
            return write_status;
        }
    }

    return Status::ok_status();
}

Status restore_regular_file(struct archive* archive_reader, const std::string& destination_path, mode_t mode) {
    const Status parent_status = ensure_parent_directories(destination_path, 0755);
    if (!parent_status.ok()) {
        return parent_status;
    }

    int destination_fd = open(
        destination_path.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC,
        static_cast<int>(mode & 0777));
    if (destination_fd < 0) {
        return make_io_error("open", destination_path);
    }

    std::vector<char> file_buffer(kStreamBufferSize);
    while (true) {
        const la_ssize_t read_count = archive_read_data(
            archive_reader, file_buffer.data(), file_buffer.size());

        if (read_count == 0) {
            break;
        }
        if (read_count < 0) {
            close(destination_fd);
            return Status(
                StatusCode::kArchiveError,
                "Failed to read archive file entry: " + std::string(archive_error_string(archive_reader)));
        }

        ssize_t total_written = 0;
        while (total_written < read_count) {
            const ssize_t write_count = write(
                destination_fd,
                file_buffer.data() + total_written,
                static_cast<size_t>(read_count - total_written));
            if (write_count < 0) {
                close(destination_fd);
                return make_io_error("write", destination_path);
            }
            total_written += write_count;
        }
    }

    close(destination_fd);
    return Status::ok_status();
}

Status restore_directory(const std::string& destination_path, mode_t mode) {
    const Status directory_status = ensure_directory_tree(destination_path, mode & 0777);
    if (!directory_status.ok()) {
        return directory_status;
    }

    return Status::ok_status();
}

}  // namespace

Status write_backup_archive(
    const std::string& data_path,
    const std::string& extensions_path,
    std::ostream& out,
    const BackupOptions& options) {
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

    OstreamWriteContext write_context;
    write_context.out = &out;

    if (archive_write_open(
            archive_writer,
            &write_context,
            archive_write_open_callback,
            archive_write_callback,
            archive_write_close_callback) != ARCHIVE_OK) {
        const std::string archive_error = archive_error_string(archive_writer);
        archive_write_free(archive_writer);
        return Status(StatusCode::kArchiveError, "Failed to open archive stream: " + archive_error);
    }

    std::set<std::string> skipped_extension_names;
    std::vector<GitExtensionInfo> git_extensions;
    if (options.git_mode_for_extensions) {
        const Status collect_status = collect_git_extension_info(
            extensions_path, &git_extensions, &skipped_extension_names);
        if (!collect_status.ok()) {
            archive_write_close(archive_writer);
            archive_write_free(archive_writer);
            return collect_status;
        }
    }

    const Status data_status = write_file_or_directory(archive_writer, data_path, "data");
    if (!data_status.ok()) {
        archive_write_close(archive_writer);
        archive_write_free(archive_writer);
        return data_status;
    }

    const Status extensions_root_status = write_directory_recursive(
        archive_writer, extensions_path, "extensions", &skipped_extension_names);
    if (!extensions_root_status.ok()) {
        archive_write_close(archive_writer);
        archive_write_free(archive_writer);
        return extensions_root_status;
    }

    if (options.git_mode_for_extensions) {
        const std::string git_manifest = build_git_manifest_json(git_extensions);
        const Status manifest_status = write_manifest_file(archive_writer, git_manifest);
        if (!manifest_status.ok()) {
            archive_write_close(archive_writer);
            archive_write_free(archive_writer);
            return manifest_status;
        }
    }

    if (archive_write_close(archive_writer) != ARCHIVE_OK) {
        const std::string archive_error = archive_error_string(archive_writer);
        archive_write_free(archive_writer);
        return Status(StatusCode::kArchiveError, "Failed to close archive writer: " + archive_error);
    }

    archive_write_free(archive_writer);
    if (!out) {
        return Status(StatusCode::kIoError, "Output stream write failed");
    }

    return Status::ok_status();
}

Status restore_backup_archive(std::istream& in, const std::string& destination_root) {
    struct archive* archive_reader = archive_read_new();
    if (archive_reader == NULL) {
        return Status(StatusCode::kArchiveError, "Failed to initialize archive reader");
    }

    archive_read_support_format_tar(archive_reader);
    archive_read_support_filter_zstd(archive_reader);

    IstreamReadContext read_context;
    read_context.in = &in;
    read_context.buffer.resize(kStreamBufferSize);

    if (archive_read_open(
            archive_reader,
            &read_context,
            archive_read_open_callback,
            archive_read_callback,
            archive_read_close_callback) != ARCHIVE_OK) {
        const std::string archive_error = archive_error_string(archive_reader);
        archive_read_free(archive_reader);
        return Status(StatusCode::kArchiveError, "Failed to open archive input stream: " + archive_error);
    }

    struct archive_entry* archive_entry = NULL;
    int next_header_status = ARCHIVE_OK;
    while ((next_header_status = archive_read_next_header(archive_reader, &archive_entry)) == ARCHIVE_OK) {
        const char* archive_path_cstr = archive_entry_pathname(archive_entry);
        if (archive_path_cstr == NULL) {
            archive_read_close(archive_reader);
            archive_read_free(archive_reader);
            return Status(StatusCode::kInvalidArchiveEntry, "Archive entry has no path");
        }

        const std::string archive_path = archive_path_cstr;
        std::string destination_path;
        const Status path_status = join_destination_path(destination_root, archive_path, &destination_path);
        if (!path_status.ok()) {
            archive_read_close(archive_reader);
            archive_read_free(archive_reader);
            return path_status;
        }

        const mode_t file_type = static_cast<mode_t>(archive_entry_filetype(archive_entry));
        const mode_t entry_mode = static_cast<mode_t>(archive_entry_perm(archive_entry));

        if (file_type == AE_IFDIR) {
            const Status directory_status = restore_directory(destination_path, entry_mode);
            if (!directory_status.ok()) {
                archive_read_close(archive_reader);
                archive_read_free(archive_reader);
                return directory_status;
            }
            continue;
        }

        if (file_type == AE_IFREG) {
            const Status file_status = restore_regular_file(archive_reader, destination_path, entry_mode);
            if (!file_status.ok()) {
                archive_read_close(archive_reader);
                archive_read_free(archive_reader);
                return file_status;
            }
            continue;
        }

        archive_read_close(archive_reader);
        archive_read_free(archive_reader);
        return Status(
            StatusCode::kUnsupportedArchiveEntry,
            "Restore supports only regular files and directories");
    }

    if (next_header_status != ARCHIVE_EOF) {
        const std::string archive_error = archive_error_string(archive_reader);
        archive_read_close(archive_reader);
        archive_read_free(archive_reader);
        return Status(StatusCode::kArchiveError, "Failed to read archive header: " + archive_error);
    }

    if (archive_read_close(archive_reader) != ARCHIVE_OK) {
        const std::string archive_error = archive_error_string(archive_reader);
        archive_read_free(archive_reader);
        return Status(StatusCode::kArchiveError, "Failed to close archive reader: " + archive_error);
    }

    archive_read_free(archive_reader);
    return Status::ok_status();
}

}  // namespace internal
}  // namespace STManager
