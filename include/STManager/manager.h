#ifndef STMANAGER_MANAGER_HPP
#define STMANAGER_MANAGER_HPP

#include <STManager/sync.h>

namespace STManager {

struct RunSyncOptions {
    ServerOptions server_options;
};

struct RunSyncResult {
    int bound_port;

    RunSyncResult() : bound_port(0) {}
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
};

struct PairSyncResult {
    DeviceInfo selected_device;
    bool paired_this_time;

    PairSyncResult() : selected_device(), paired_this_time(false) {}
};

class STMANAGER_EXPORT Manager {
public:
    Manager();

    static Status create_from_root(const std::string& root_path, Manager* manager_out);

    const std::string& root_path() const;
    const std::string& local_device_id() const;
    const std::string& state_dir() const;

    Status discover_devices(std::vector<DeviceInfo>* devices) const;
    Status resolve_pair_target(
        const PairSyncRequest& request,
        std::vector<DeviceInfo>* candidates,
        DeviceInfo* auto_selected) const;

    Status run_sync(const RunSyncOptions& options, RunSyncResult* result) const;
    Status pair_sync(
        const DeviceInfo& device_info,
        const PairSyncOptions& options,
        PairSyncResult* result) const;

private:
    Status initialize_from_root(const std::string& root_path);
    Status ensure_initialized() const;

    DataManager data_manager_;
    std::string state_dir_;
    std::string local_device_id_;
    std::string trusted_store_path_;
    bool initialized_;
};

}  // namespace STManager

#endif
