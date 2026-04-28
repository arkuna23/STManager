#ifndef STMANAGER_MANAGER_HPP
#define STMANAGER_MANAGER_HPP

#include <STManager/sync.h>

#include <cstdint>
#include <iosfwd>
#include <memory>

namespace STManager {

/** Options for Manager::serve_sync. */
struct ServeSyncOptions {
    ServerOptions server_options;
    std::string device_name;

    ServeSyncOptions(
        const std::string& bind_host_in = "0.0.0.0",
        int port_in = 0,
        const std::string& pairing_code_in = std::string(),
        bool advertise_in = true,
        const std::string& advertise_name_in = std::string(),
        const std::string& device_name_in = std::string())
        : server_options(),
          device_name(device_name_in) {
        server_options.bind_host = bind_host_in;
        server_options.port = port_in;
        server_options.pairing_code = pairing_code_in;
        server_options.advertise = advertise_in;
        server_options.advertise_name = advertise_name_in;
    }
};

/** Legacy serve result retained for source compatibility. */
struct ServeSyncResult {
    int bound_port;

    ServeSyncResult() : bound_port(0) {}
};

/** Pair target supplied by a caller or resolved from discovery. */
struct PairSyncRequest {
    std::string device_id;
    std::string host;
    int port;

    PairSyncRequest() : device_id(), host(), port(0) {}
};

/** Options for Manager::pair_sync. */
struct PairSyncOptions {
    PairingOptions pairing_options;
    SyncOptions sync_options;
    std::string device_name;

    PairSyncOptions(
        const std::string& pairing_code_in = std::string(),
        bool remember_device_in = true,
        const std::string& destination_root_override_in = std::string(),
        const BackupOptions& backup_options_in = BackupOptions(),
        const std::string& device_name_in = std::string())
        : pairing_options(),
          sync_options(),
          device_name(device_name_in) {
        pairing_options.pairing_code = pairing_code_in;
        pairing_options.remember_device = remember_device_in;
        sync_options.destination_root_override = destination_root_override_in;
        sync_options.backup_options = backup_options_in;
    }
};

/** Result returned after pair + pull restore completes. */
struct PairSyncResult {
    DeviceInfo selected_device;
    bool paired_this_time;

    PairSyncResult()
        : selected_device(),
          paired_this_time(false) {}
};

/** File-based backup export options. */
struct ExportBackupOptions {
    std::string file_path;
    BackupOptions backup_options;

    ExportBackupOptions() : file_path("st-backup.tar.zst"), backup_options() {}
};

/** File-based backup export result. */
struct ExportBackupResult {
    std::string file_path;
    uint64_t bytes_written;

    ExportBackupResult() : file_path(), bytes_written(0) {}
};

/** File-based backup restore options. */
struct RestoreBackupOptions {
    std::string file_path;

    RestoreBackupOptions() : file_path("st-backup.tar.zst") {}
};

/** Mode represented by a SyncTaskHandle. */
enum class SyncTaskMode {
    kServe = 0,
    kPair,
};

/** Lifecycle state of an asynchronous sync task. */
enum class SyncTaskState {
    kStarting = 0,
    kRunning,
    kStopping,
    kFinished,
};

/** Handle returned by asynchronous sync operations. */
class STMANAGER_EXPORT SyncTaskHandle {
public:
    class Impl;

    SyncTaskHandle();
    ~SyncTaskHandle();

    SyncTaskHandle(const SyncTaskHandle&) = delete;
    SyncTaskHandle& operator=(const SyncTaskHandle&) = delete;

    SyncTaskHandle(SyncTaskHandle&& other);
    SyncTaskHandle& operator=(SyncTaskHandle&& other);
    explicit SyncTaskHandle(const std::shared_ptr<Impl>& impl);

    /** Request the task to stop. The task may finish asynchronously. */
    void stop();

    /** Wait for the task to finish and return its final status. */
    Status wait();

    /** Return the current task state. */
    SyncTaskState state() const;

    /** Return whether this task is serving or pairing. */
    SyncTaskMode mode() const;

    /** Return the latest task status, or final status after completion. */
    Status last_status() const;

    /** Return local task/device information such as id, name, host, and port. */
    DeviceInfo info() const;

    /** Return true while the task is starting or running. */
    bool is_running() const;

private:
    std::shared_ptr<Impl> impl_;
};

class STMANAGER_EXPORT Manager {
public:
    Manager();

    /**
     * Create a Manager from a SillyTavern root.
     *
     * On success, manager_out owns state under the root's .stmanager directory.
     */
    static Status create_from_root(const std::string& root_path, Manager* manager_out);

    /** Return the managed SillyTavern root path. */
    const std::string& root_path() const;

    /** Return the persistent local device id. */
    const std::string& local_device_id() const;

    /** Return the local device display name. */
    const std::string& local_device_name() const;

    /** Return the .stmanager state directory path. */
    const std::string& state_dir() const;

    /** Discover available devices on the current LAN. */
    Status discover_devices(std::vector<DeviceInfo>* devices) const;

    /**
     * Resolve a pair target from request.
     *
     * Direct host/port requests are selected immediately. Empty requests use
     * discovery and return candidates for interactive selection.
     */
    Status resolve_pair_target(
        const PairSyncRequest& request,
        std::vector<DeviceInfo>* candidates,
        DeviceInfo* auto_selected) const;

    /**
     * Start serving local backup data in a background thread.
     *
     * The returned handle owns the task. Call stop() then wait() to shut it down.
     */
    std::unique_ptr<SyncTaskHandle> serve_sync(
        const ServeSyncOptions& options = ServeSyncOptions()) const;

    /**
     * Pair with device_info if needed, then pull and restore its backup data.
     *
     * This call runs synchronously in the current thread.
     */
    Status pair_sync(
        const DeviceInfo& device_info,
        const PairSyncOptions& options,
        PairSyncResult* result) const;

    /**
     * Export a backup archive to an output stream.
     *
     * The caller owns out. bytes_written is optional and receives the archive
     * byte count for data written through this API.
     */
    Status export_backup(
        std::ostream& out,
        const BackupOptions& backup_options = BackupOptions(),
        uint64_t* bytes_written = NULL) const;

    /**
     * Export a backup archive to a file descriptor.
     *
     * The caller owns fd; this function never closes it. bytes_written is
     * optional. On Windows builds, fd export returns an unsupported status.
     */
    Status export_backup_to_fd(
        int fd,
        const BackupOptions& backup_options = BackupOptions(),
        uint64_t* bytes_written = NULL) const;

    /** Export a backup archive to options.file_path. */
    Status export_backup(const ExportBackupOptions& options, ExportBackupResult* result) const;

    /**
     * Restore a backup archive from an input stream.
     *
     * The caller owns in. Seekable streams are rewound to the beginning before
     * restore; non-seekable streams are consumed from their current position.
     */
    Status restore_backup(std::istream& in) const;

    /**
     * Restore a backup archive from a file descriptor.
     *
     * The caller owns fd; this function never closes it. Seekable descriptors
     * are rewound before restore. On Windows builds, fd restore returns an
     * unsupported status.
     */
    Status restore_backup_from_fd(int fd) const;

    /** Restore a backup archive from options.file_path. */
    Status restore_backup(const RestoreBackupOptions& options) const;

private:
    Status initialize_from_root(const std::string& root_path);
    Status ensure_initialized() const;

    DataManager data_manager_;
    std::string state_dir_;
    std::string local_device_id_;
    std::string local_device_name_;
    std::string trusted_store_path_;
    bool initialized_;
};

}  // namespace STManager

#endif
