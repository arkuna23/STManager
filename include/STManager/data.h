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

/** Result code and human-readable diagnostic returned by all public APIs. */
struct Status {
    StatusCode code;
    std::string message;

    Status() : code(StatusCode::kOk), message() {}
    Status(StatusCode code_in, const std::string& message_in)
        : code(code_in), message(message_in) {}

    bool ok() const { return code == StatusCode::kOk; }

    static Status ok_status() { return Status(); }
};

/** Backup creation options shared by file, stream, fd, and sync APIs. */
struct BackupOptions {
    bool git_mode_for_extensions;

    BackupOptions() : git_mode_for_extensions(false) {}
};

/** Restore options reserved for future restore behavior switches. */
struct RestoreOptions {};

/** Git metadata emitted when extension backup runs in git mode. */
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

    /**
     * Locate a SillyTavern root and resolve the managed data directories.
     *
     * The root must contain the SillyTavern data directory. The third-party
     * extension directory is created when the broader extensions tree exists.
     */
    static DataManager locate(const std::string& root_path);

    /** Returns true when locate/constructor resolved a usable SillyTavern root. */
    bool is_valid() const;

    /** Returns the last locate/validation status for this manager. */
    const Status& last_status() const;

    /**
     * Write a tar+zstd backup to out.
     *
     * The caller owns out and is responsible for opening/closing it.
     */
    Status backup(std::ostream& out) const;

    /**
     * Write a tar+zstd backup to out with explicit backup options.
     *
     * The caller owns out and is responsible for opening/closing it.
     */
    Status backup(std::ostream& out, const BackupOptions& options) const;

    /**
     * Restore a backup archive from in into destination_root.
     *
     * Restore is full replacement for managed data and third-party extensions.
     * The caller owns in and is responsible for positioning it at backup data.
     */
    Status restore(std::istream& in, const std::string& destination_root) const;

    /**
     * Restore a backup archive from in with explicit restore options.
     *
     * Restore is full replacement for managed data and third-party extensions.
     */
    Status restore(
        std::istream& in,
        const std::string& destination_root,
        const RestoreOptions& options) const;

private:
    Status status_;

    explicit DataManager(const Status& status);
    DataManager(const std::string& root_path_in, const std::string& extensions_path_in,
                const std::string& data_path_in, const Status& status);
};

}  // namespace STManager

#endif
