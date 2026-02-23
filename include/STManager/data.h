#ifndef STMANAGER_DATA_MANAGER_HPP
#define STMANAGER_DATA_MANAGER_HPP

#include <STManager/stmanager_export.h>

#include <iosfwd>
#include <string>

namespace STManager {

enum class StatusCode {
    kOk = 0,
    kInvalidRoot,
    kMissingExtensionsDir,
    kMissingDataDir,
    kIoError,
    kArchiveError,
    kInvalidArchiveEntry,
    kUnsupportedArchiveEntry,
    kDiscoveryError,
    kSyncProtocolError,
    kUnauthorized,
};

struct Status {
    StatusCode code;
    std::string message;

    Status() : code(StatusCode::kOk), message() {}
    Status(StatusCode code_in, const std::string& message_in)
        : code(code_in), message(message_in) {}

    bool ok() const { return code == StatusCode::kOk; }

    static Status ok_status() { return Status(); }
};

struct BackupOptions {
    bool git_mode_for_extensions;

    BackupOptions() : git_mode_for_extensions(false) {}
};

struct GitExtensionInfo {
    std::string extension_name;
    std::string remote_url;
};

class STMANAGER_EXPORT DataManager {
public:
    std::string root_path;
    std::string extensions_path;
    std::string data_path;

    DataManager();
    explicit DataManager(const std::string& root_path_in);

    static DataManager locate(const std::string& root_path);

    bool is_valid() const;
    const Status& last_status() const;

    Status backup(std::ostream& out) const;
    Status backup(std::ostream& out, const BackupOptions& options) const;
    Status restore(std::istream& in, const std::string& destination_root) const;

private:
    Status status_;

    explicit DataManager(const Status& status);
    DataManager(const std::string& root_path_in, const std::string& extensions_path_in,
                const std::string& data_path_in, const Status& status);
};

}  // namespace STManager

#endif
