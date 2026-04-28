#ifndef STMANAGER_SYNC_HPP
#define STMANAGER_SYNC_HPP

#include <STManager/data.h>
#include <STManager/stmanager_export.h>

#include <atomic>
#include <iosfwd>
#include <string>
#include <vector>

namespace STManager {

enum class SyncDirection {
    kPush = 0,
    kPull,
};

/** Network-visible identity and endpoint for a sync-capable device. */
struct DeviceInfo {
    std::string device_id;
    std::string device_name;
    std::string host;
    int port;

    DeviceInfo() : device_id(), device_name(), host(), port(0) {}
};

/** Options used when pushing or pulling backup data between devices. */
struct SyncOptions {
    BackupOptions backup_options;
    std::string destination_root_override;

    SyncOptions() : backup_options(), destination_root_override() {}
};

/** Pairing options used when trusting a remote device. */
struct PairingOptions {
    std::string pairing_code;
    bool remember_device;

    PairingOptions() : pairing_code(), remember_device(true) {}
};

/** Options for serving backup data over the built-in sync server. */
struct ServerOptions {
    std::string bind_host;
    int port;
    std::string pairing_code;
    bool advertise;
    std::string advertise_name;

    ServerOptions()
        : bind_host("0.0.0.0"), port(0), pairing_code(), advertise(true), advertise_name() {}
};

/** Transport abstraction used by SyncManager for tests and custom transports. */
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

/** Persistent trust store abstraction for paired devices. */
class STMANAGER_EXPORT ITrustedDeviceStore {
public:
    virtual ~ITrustedDeviceStore() {}

    virtual Status load() = 0;
    virtual Status save() const = 0;

    virtual bool is_trusted(const std::string& device_id) const = 0;
    virtual Status trust_device(const std::string& device_id) = 0;
    virtual Status untrust_device(const std::string& device_id) = 0;
};

/** Coordinates pair, push, pull, and discovery using injected dependencies. */
class STMANAGER_EXPORT SyncManager {
public:
    /**
     * Create a sync manager around an already-located DataManager.
     *
     * transport, discovery, and trusted_store are borrowed and must outlive the
     * SyncManager instance.
     */
    SyncManager(const DataManager& data_manager, const std::string& local_device_id,
                const std::string& local_device_name,
                ISyncTransport* transport, IDeviceDiscovery* discovery,
                ITrustedDeviceStore* trusted_store);

    /** Discover available devices using the configured discovery backend. */
    Status discover_devices(std::vector<DeviceInfo>* devices) const;

    /** Pair with a remote device and optionally remember it in the trust store. */
    Status pair_device(const DeviceInfo& device_info, const PairingOptions& options);

    /** Push local backup data to a trusted remote device. */
    Status push_to_device(const DeviceInfo& device_info, const SyncOptions& options) const;

    /** Pull backup data from a trusted remote device and restore it locally. */
    Status pull_from_device(const DeviceInfo& device_info, const SyncOptions& options) const;

    /** Run push or pull according to direction. */
    Status sync(SyncDirection direction, const DeviceInfo& device_info,
                const SyncOptions& options) const;

private:
    const DataManager& data_manager_;
    std::string local_device_id_;
    std::string local_device_name_;
    ISyncTransport* transport_;
    IDeviceDiscovery* discovery_;
    ITrustedDeviceStore* trusted_store_;

    Status authorize_remote(const DeviceInfo& device_info, SyncDirection direction) const;
};

class STMANAGER_EXPORT MdnsDeviceDiscovery : public IDeviceDiscovery {
public:
    MdnsDeviceDiscovery();

    /** Start the built-in LAN discovery responder/listener. */
    Status start() override;

    /** Stop the built-in LAN discovery responder/listener. */
    Status stop() override;

    /** List devices discovered on the local network. */
    Status list_devices(std::vector<DeviceInfo>* devices) const override;

private:
    bool is_running_;
};

class STMANAGER_EXPORT JsonTrustedDeviceStore : public ITrustedDeviceStore {
public:
    /** Use store_path as the JSON file backing the trusted device list. */
    explicit JsonTrustedDeviceStore(const std::string& store_path);

    /** Load trusted devices from disk. */
    Status load() override;

    /** Save trusted devices to disk. */
    Status save() const override;

    /** Return whether device_id is currently trusted. */
    bool is_trusted(const std::string& device_id) const override;

    /** Mark device_id as trusted. */
    Status trust_device(const std::string& device_id) override;

    /** Remove device_id from the trusted device list. */
    Status untrust_device(const std::string& device_id) override;

private:
    std::string store_path_;
    std::vector<std::string> trusted_device_ids_;
};

typedef void (*ServeSyncStartedCallback)(
    const std::string& bound_host,
    int bound_port,
    void* user_context);

/**
 * Run the sync server in the current thread.
 *
 * The server serves backups to paired/trusted clients until stop_requested is
 * set, the listening socket is interrupted, or an unrecoverable error occurs.
 */
STMANAGER_EXPORT Status serve_sync_server(const DataManager& data_manager,
                                          const std::string& local_device_id,
                                          ITrustedDeviceStore* trusted_store,
                                          const ServerOptions& options,
                                          int* bound_port,
                                          std::string* bound_host = NULL,
                                          const std::atomic<bool>* stop_requested = NULL,
                                          ServeSyncStartedCallback started_callback = NULL,
                                          void* started_context = NULL);

}  // namespace STManager

#endif
