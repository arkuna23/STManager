#include <STManager/sync.h>

#include "discovery_mdns.h"
#include "sync_protocol.h"
#include "trusted_device_store.h"

#include <sstream>

namespace STManager {

SyncManager::SyncManager(
    const DataManager& data_manager,
    const std::string& local_device_id,
    ISyncTransport* transport,
    IDeviceDiscovery* discovery,
    ITrustedDeviceStore* trusted_store)
    : data_manager_(data_manager),
      local_device_id_(local_device_id),
      transport_(transport),
      discovery_(discovery),
      trusted_store_(trusted_store) {}

Status SyncManager::discover_devices(std::vector<DeviceInfo>* devices) const {
    if (discovery_ == NULL) {
        return Status(StatusCode::kDiscoveryError, "Device discovery is not configured");
    }

    const Status start_status = discovery_->start();
    if (!start_status.ok()) {
        return start_status;
    }

    return discovery_->list_devices(devices);
}

Status SyncManager::pair_device(const DeviceInfo& device_info, const PairingOptions& options) {
    if (transport_ == NULL || trusted_store_ == NULL) {
        return Status(StatusCode::kSyncProtocolError, "Transport or trusted device store is not configured");
    }

    const Status load_status = trusted_store_->load();
    if (!load_status.ok()) {
        return load_status;
    }

    const Status connect_status = transport_->connect(device_info);
    if (!connect_status.ok()) {
        return connect_status;
    }

    const std::string pair_request_message =
        internal::build_pair_request_message(local_device_id_, options);
    const Status send_status = transport_->send_message(pair_request_message);
    if (!send_status.ok()) {
        transport_->disconnect();
        return send_status;
    }

    std::string pair_response_message;
    const Status receive_status = transport_->receive_message(&pair_response_message);
    if (!receive_status.ok()) {
        transport_->disconnect();
        return receive_status;
    }

    bool is_accepted = false;
    std::string response_error;
    const Status parse_status =
        internal::parse_pair_response_message(pair_response_message, &is_accepted, &response_error);
    if (!parse_status.ok()) {
        transport_->disconnect();
        return parse_status;
    }

    transport_->disconnect();

    if (!is_accepted) {
        if (response_error.empty()) {
            response_error = "Pairing rejected by remote device";
        }
        return Status(StatusCode::kUnauthorized, response_error);
    }

    if (!options.remember_device) {
        return Status::ok_status();
    }

    const Status trust_status = trusted_store_->trust_device(device_info.device_id);
    if (!trust_status.ok()) {
        return trust_status;
    }

    return trusted_store_->save();
}

Status SyncManager::authorize_remote(const DeviceInfo& device_info, SyncDirection direction) const {
    if (transport_ == NULL || trusted_store_ == NULL) {
        return Status(StatusCode::kSyncProtocolError, "Transport or trusted device store is not configured");
    }

    const Status load_status = trusted_store_->load();
    if (!load_status.ok()) {
        return load_status;
    }

    if (!trusted_store_->is_trusted(device_info.device_id)) {
        return Status(StatusCode::kUnauthorized, "Device is not trusted. Pairing is required before sync");
    }

    const std::string auth_request_message =
        internal::build_auth_request_message(local_device_id_, direction);
    const Status send_status = transport_->send_message(auth_request_message);
    if (!send_status.ok()) {
        return send_status;
    }

    std::string auth_response_message;
    const Status receive_status = transport_->receive_message(&auth_response_message);
    if (!receive_status.ok()) {
        return receive_status;
    }

    bool is_accepted = false;
    std::string response_error;
    const Status parse_status =
        internal::parse_auth_response_message(auth_response_message, &is_accepted, &response_error);
    if (!parse_status.ok()) {
        return parse_status;
    }

    if (!is_accepted) {
        if (response_error.empty()) {
            response_error = "Remote authorization was rejected";
        }
        return Status(StatusCode::kUnauthorized, response_error);
    }

    return Status::ok_status();
}

Status SyncManager::push_to_device(const DeviceInfo& device_info, const SyncOptions& options) const {
    if (!data_manager_.is_valid()) {
        return data_manager_.last_status();
    }

    if (transport_ == NULL) {
        return Status(StatusCode::kSyncProtocolError, "Transport is not configured");
    }

    const Status connect_status = transport_->connect(device_info);
    if (!connect_status.ok()) {
        return connect_status;
    }

    const Status auth_status = authorize_remote(device_info, SyncDirection::kPush);
    if (!auth_status.ok()) {
        transport_->disconnect();
        return auth_status;
    }

    std::stringstream backup_stream(std::ios::in | std::ios::out | std::ios::binary);
    const Status backup_status = data_manager_.backup(backup_stream, options.backup_options);
    if (!backup_status.ok()) {
        transport_->disconnect();
        return backup_status;
    }

    backup_stream.seekg(0, std::ios::beg);
    const Status send_status = transport_->send_stream(backup_stream);
    transport_->disconnect();

    return send_status;
}

Status SyncManager::pull_from_device(const DeviceInfo& device_info, const SyncOptions& options) const {
    if (!data_manager_.is_valid()) {
        return data_manager_.last_status();
    }

    if (transport_ == NULL) {
        return Status(StatusCode::kSyncProtocolError, "Transport is not configured");
    }

    const Status connect_status = transport_->connect(device_info);
    if (!connect_status.ok()) {
        return connect_status;
    }

    const Status auth_status = authorize_remote(device_info, SyncDirection::kPull);
    if (!auth_status.ok()) {
        transport_->disconnect();
        return auth_status;
    }

    std::stringstream backup_stream(std::ios::in | std::ios::out | std::ios::binary);
    const Status receive_status = transport_->receive_stream(backup_stream);
    if (!receive_status.ok()) {
        transport_->disconnect();
        return receive_status;
    }

    backup_stream.seekg(0, std::ios::beg);

    const std::string restore_root = options.destination_root_override.empty()
        ? data_manager_.root_path
        : options.destination_root_override;

    const Status restore_status = data_manager_.restore(backup_stream, restore_root);
    transport_->disconnect();
    return restore_status;
}

Status SyncManager::sync(
    SyncDirection direction,
    const DeviceInfo& device_info,
    const SyncOptions& options) const {
    if (direction == SyncDirection::kPush) {
        return push_to_device(device_info, options);
    }

    return pull_from_device(device_info, options);
}

MdnsDeviceDiscovery::MdnsDeviceDiscovery() : is_running_(false) {}

Status MdnsDeviceDiscovery::start() {
    is_running_ = true;
    return Status::ok_status();
}

Status MdnsDeviceDiscovery::stop() {
    is_running_ = false;
    return Status::ok_status();
}

Status MdnsDeviceDiscovery::list_devices(std::vector<DeviceInfo>* devices) const {
    if (!is_running_) {
        return Status(StatusCode::kDiscoveryError, "Discovery is not running");
    }

    return internal::list_mdns_devices(devices);
}

JsonTrustedDeviceStore::JsonTrustedDeviceStore(const std::string& store_path)
    : store_path_(store_path), trusted_device_ids_() {}

Status JsonTrustedDeviceStore::load() {
    return internal::load_trusted_device_ids(store_path_, &trusted_device_ids_);
}

Status JsonTrustedDeviceStore::save() const {
    return internal::save_trusted_device_ids(store_path_, trusted_device_ids_);
}

bool JsonTrustedDeviceStore::is_trusted(const std::string& device_id) const {
    return internal::contains_device_id(trusted_device_ids_, device_id);
}

Status JsonTrustedDeviceStore::trust_device(const std::string& device_id) {
    if (device_id.empty()) {
        return Status(StatusCode::kSyncProtocolError, "Device ID cannot be empty");
    }

    internal::add_device_id(&trusted_device_ids_, device_id);
    return Status::ok_status();
}

Status JsonTrustedDeviceStore::untrust_device(const std::string& device_id) {
    internal::remove_device_id(&trusted_device_ids_, device_id);
    return Status::ok_status();
}

}  // namespace STManager
