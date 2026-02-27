#include <STManager/data.h>

#include "archive_stream.h"
#include "locate.h"

#include <exception>
#include <istream>
#include <ostream>

namespace STManager {

DataManager::DataManager() : root_path(), extensions_path(), data_path(), status_(Status::ok_status()) {}

DataManager::DataManager(const std::string& root_path_in)
    : root_path(root_path_in),
      extensions_path(),
      data_path(),
      status_(Status(StatusCode::kInvalidRoot, "Unresolved SillyTavern path")) {}

DataManager::DataManager(const Status& status)
    : root_path(), extensions_path(), data_path(), status_(status) {}

DataManager::DataManager(
    const std::string& root_path_in,
    const std::string& extensions_path_in,
    const std::string& data_path_in,
    const Status& status)
    : root_path(root_path_in),
      extensions_path(extensions_path_in),
      data_path(data_path_in),
      status_(status) {}

DataManager DataManager::locate(const std::string& root_path) {
    std::string resolved_root_path;
    std::string extensions_path;
    std::string data_path;

    const Status locate_status = internal::locate_silly_tavern_paths(
        root_path,
        &resolved_root_path,
        &extensions_path,
        &data_path);

    if (!locate_status.ok()) {
        return DataManager(locate_status);
    }

    return DataManager(resolved_root_path, extensions_path, data_path, Status::ok_status());
}

bool DataManager::is_valid() const { return status_.ok(); }

const Status& DataManager::last_status() const { return status_; }

Status DataManager::backup(std::ostream& out) const {
    return backup(out, BackupOptions());
}

Status DataManager::backup(std::ostream& out, const BackupOptions& options) const {
    if (!status_.ok()) {
        return status_;
    }

    try {
        return internal::write_backup_archive(data_path, extensions_path, out, options);
    } catch (const std::exception& exception) {
        return Status(
            StatusCode::kIoError,
            std::string("stage=data.backup.exception; ") + exception.what());
    } catch (...) {
        return Status(
            StatusCode::kIoError,
            "stage=data.backup.exception; unknown exception");
    }
}

Status DataManager::restore(std::istream& in, const std::string& destination_root) const {
    return restore(in, destination_root, RestoreOptions());
}

Status DataManager::restore(
    std::istream& in,
    const std::string& destination_root,
    const RestoreOptions& options) const {
    if (!status_.ok()) {
        return status_;
    }

    try {
        return internal::restore_backup_archive(in, destination_root, options);
    } catch (const std::exception& exception) {
        return Status(
            StatusCode::kIoError,
            std::string("stage=data.restore.exception; ") + exception.what());
    } catch (...) {
        return Status(
            StatusCode::kIoError,
            "stage=data.restore.exception; unknown exception");
    }
}

}  // namespace STManager
