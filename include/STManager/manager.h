#ifndef STMANAGER_MANAGER_HPP
#define STMANAGER_MANAGER_HPP

#include <STManager/sync.h>

#include <cstdint>
#include <iosfwd>
#include <memory>

namespace STManager {

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

struct ServeSyncResult {
    int bound_port;

    ServeSyncResult() : bound_port(0) {}
};

struct PairSyncRequest {
    std::string device_id;
    std::string host;
    int port;

    PairSyncRequest() : device_id(), host(), port(0) {}
};

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

struct PairSyncResult {
    DeviceInfo selected_device;
    bool paired_this_time;

    PairSyncResult()
        : selected_device(),
          paired_this_time(false) {}
};

struct ExportBackupOptions {
    std::string file_path;
    BackupOptions backup_options;

    ExportBackupOptions() : file_path("st-backup.tar.zst"), backup_options() {}
};

struct ExportBackupResult {
    std::string file_path;
    uint64_t bytes_written;

    ExportBackupResult() : file_path(), bytes_written(0) {}
};

struct RestoreBackupOptions {
    std::string file_path;

    RestoreBackupOptions() : file_path("st-backup.tar.zst") {}
};

enum class SyncTaskMode {
    kServe = 0,
    kPair,
};

enum class SyncTaskState {
    kStarting = 0,
    kRunning,
    kStopping,
    kFinished,
};

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

    void stop();
    Status wait();

    SyncTaskState state() const;
    SyncTaskMode mode() const;
    Status last_status() const;
    DeviceInfo info() const;
    bool is_running() const;

private:
    std::shared_ptr<Impl> impl_;
};

class STMANAGER_EXPORT Manager {
public:
    Manager();

    static Status create_from_root(const std::string& root_path, Manager* manager_out);

    const std::string& root_path() const;
    const std::string& local_device_id() const;
    const std::string& local_device_name() const;
    const std::string& state_dir() const;

    Status discover_devices(std::vector<DeviceInfo>* devices) const;
    Status resolve_pair_target(
        const PairSyncRequest& request,
        std::vector<DeviceInfo>* candidates,
        DeviceInfo* auto_selected) const;

    std::unique_ptr<SyncTaskHandle> serve_sync(
        const ServeSyncOptions& options = ServeSyncOptions()) const;
    Status pair_sync(
        const DeviceInfo& device_info,
        const PairSyncOptions& options,
        PairSyncResult* result) const;
    Status export_backup(
        std::ostream& out,
        const BackupOptions& backup_options = BackupOptions(),
        uint64_t* bytes_written = NULL) const;
    Status export_backup_to_fd(
        int fd,
        const BackupOptions& backup_options = BackupOptions(),
        uint64_t* bytes_written = NULL) const;
    Status export_backup(const ExportBackupOptions& options, ExportBackupResult* result) const;
    Status restore_backup(std::istream& in) const;
    Status restore_backup_from_fd(int fd) const;
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
