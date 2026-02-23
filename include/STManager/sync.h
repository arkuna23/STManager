#ifndef STMANAGER_SYNC_HPP
#define STMANAGER_SYNC_HPP

#include <STManager/data.h>
#include <STManager/stmanager_export.h>

#include <iosfwd>
#include <string>
#include <vector>

namespace STManager {

enum class SyncDirection {
    kPush = 0,
    kPull,
};

struct DeviceInfo {
    std::string device_id;
    std::string device_name;
    std::string host;
    int port;

    DeviceInfo() : device_id(), device_name(), host(), port(0) {}
};

struct SyncOptions {
    BackupOptions backup_options;
    std::string destination_root_override;
};

struct PairingOptions {
    std::string pairing_code;
    bool remember_device;

    PairingOptions() : pairing_code(), remember_device(true) {}
};

struct ServerOptions {
    std::string bind_host;
    int port;
    std::string pairing_code;
    bool advertise;
    std::string advertise_name;

    ServerOptions() : bind_host("0.0.0.0"), port(0), pairing_code(), advertise(true), advertise_name() {}
};

class STMANAGER_EXPORT ISyncTransport {
public:
    virtual ~ISyncTransport() {}

    virtual Status connect(const DeviceInfo& device_info) = 0;
    virtual Status disconnect() = 0;

    virtual Status send_message(const std::string& message) = 0;
    virtual Status receive_message(std::string* message) = 0;

    virtual Status send_stream(std::istream& in) = 0;
    virtual Status receive_stream(std::ostream& out) = 0;
};

class STMANAGER_EXPORT IDeviceDiscovery {
public:
    virtual ~IDeviceDiscovery() {}

    virtual Status start() = 0;
    virtual Status stop() = 0;
    virtual Status list_devices(std::vector<DeviceInfo>* devices) const = 0;
};

class STMANAGER_EXPORT ITrustedDeviceStore {
public:
    virtual ~ITrustedDeviceStore() {}

    virtual Status load() = 0;
    virtual Status save() const = 0;

    virtual bool is_trusted(const std::string& device_id) const = 0;
    virtual Status trust_device(const std::string& device_id) = 0;
    virtual Status untrust_device(const std::string& device_id) = 0;
};

class STMANAGER_EXPORT SyncManager {
public:
    SyncManager(
        const DataManager& data_manager,
        const std::string& local_device_id,
        ISyncTransport* transport,
        IDeviceDiscovery* discovery,
        ITrustedDeviceStore* trusted_store);

    Status discover_devices(std::vector<DeviceInfo>* devices) const;
    Status pair_device(const DeviceInfo& device_info, const PairingOptions& options);

    Status push_to_device(const DeviceInfo& device_info, const SyncOptions& options) const;
    Status pull_from_device(const DeviceInfo& device_info, const SyncOptions& options) const;

    Status sync(SyncDirection direction, const DeviceInfo& device_info, const SyncOptions& options) const;

private:
    const DataManager& data_manager_;
    std::string local_device_id_;
    ISyncTransport* transport_;
    IDeviceDiscovery* discovery_;
    ITrustedDeviceStore* trusted_store_;

    Status authorize_remote(const DeviceInfo& device_info, SyncDirection direction) const;
};

class STMANAGER_EXPORT MdnsDeviceDiscovery : public IDeviceDiscovery {
public:
    MdnsDeviceDiscovery();

    Status start() override;
    Status stop() override;
    Status list_devices(std::vector<DeviceInfo>* devices) const override;

private:
    bool is_running_;
};

class STMANAGER_EXPORT JsonTrustedDeviceStore : public ITrustedDeviceStore {
public:
    explicit JsonTrustedDeviceStore(const std::string& store_path);

    Status load() override;
    Status save() const override;

    bool is_trusted(const std::string& device_id) const override;
    Status trust_device(const std::string& device_id) override;
    Status untrust_device(const std::string& device_id) override;

private:
    std::string store_path_;
    std::vector<std::string> trusted_device_ids_;
};

STMANAGER_EXPORT Status run_sync_server(
    const DataManager& data_manager,
    const std::string& local_device_id,
    ITrustedDeviceStore* trusted_store,
    const ServerOptions& options,
    int* bound_port);

}  // namespace STManager

#endif
