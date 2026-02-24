#include <STManager/manager.h>
#include <STManager/tcp_transport.h>

#include "discovery_mdns.h"
#include "platform_compat.h"
#include "path_safety.h"
#include "sync_protocol.h"
#include "trusted_device_store.h"

#include <atomic>
#include <ctime>
#include <functional>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace STManager {
namespace {

const int kDefaultSyncPort = 38591;
const char* kStateDirectoryName = ".stmanager";
const char* kDeviceIdFileName = "device_id";
const char* kTrustedStoreFileName = "trusted_devices.json";

std::string join_path(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (lhs[lhs.size() - 1] == '/') {
        return lhs + rhs;
    }
    return lhs + "/" + rhs;
}

long get_process_id() {
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

std::string generate_device_id() {
    std::ostringstream out;
    out << "device-" << get_process_id() << "-" << static_cast<long>(std::time(NULL));
    return out.str();
}

Status load_or_create_device_id(const std::string& device_id_path, std::string* device_id) {
    if (device_id == NULL) {
        return Status(StatusCode::kSyncProtocolError, "device_id output cannot be null");
    }

    std::ifstream in_file(device_id_path.c_str());
    if (in_file.is_open()) {
        std::getline(in_file, *device_id);
        if (!device_id->empty()) {
            return Status::ok_status();
        }
    }

    *device_id = generate_device_id();
    std::ofstream out_file(device_id_path.c_str(), std::ios::trunc);
    if (!out_file.is_open()) {
        return Status(StatusCode::kIoError, "Failed opening device id file for write");
    }

    out_file << *device_id << "\n";
    if (!out_file) {
        return Status(StatusCode::kIoError, "Failed writing device id file");
    }

    return Status::ok_status();
}

bool is_connectable_host(const std::string& host) {
    return !host.empty() && host != "0.0.0.0";
}

Status make_sync_canceled_status() {
    return Status(StatusCode::kSyncProtocolError, "Sync task canceled");
}

}  // namespace

class SyncTaskHandle::Impl {
public:
    explicit Impl(const SyncTaskInfo& task_info)
        : task_state_(SyncTaskState::kStarting),
          task_status_(Status::ok_status()),
          task_info_(task_info),
          stop_requested_(false),
          worker_thread_(),
          interrupt_callback_() {}

    ~Impl() {
        request_stop();
        wait();
    }

    void start(const std::function<Status()>& worker_fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_thread_ = std::thread(&Impl::run, this, worker_fn);
    }

    void mark_finished(const Status& task_status) {
        std::lock_guard<std::mutex> lock(mutex_);
        task_status_ = task_status;
        task_state_ = SyncTaskState::kFinished;
        interrupt_callback_ = std::function<void()>();
    }

    void set_interrupt_callback(const std::function<void()>& interrupt_callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        interrupt_callback_ = interrupt_callback;
    }

    void set_local_endpoint(const std::string& local_ip, int local_port) {
        std::lock_guard<std::mutex> lock(mutex_);
        task_info_.local_ip = local_ip;
        task_info_.local_port = local_port;
    }

    void request_stop() {
        stop_requested_.store(true);

        std::function<void()> interrupt_callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (task_state_ == SyncTaskState::kStarting || task_state_ == SyncTaskState::kRunning) {
                task_state_ = SyncTaskState::kStopping;
            }
            interrupt_callback = interrupt_callback_;
        }

        if (interrupt_callback) {
            interrupt_callback();
        }
    }

    bool stop_requested() const {
        return stop_requested_.load();
    }

    const std::atomic<bool>* stop_requested_ptr() const {
        return &stop_requested_;
    }

    Status wait() {
        std::thread worker_thread;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (worker_thread_.joinable()) {
                if (worker_thread_.get_id() == std::this_thread::get_id()) {
                    return Status(StatusCode::kSyncProtocolError, "cannot wait from worker thread");
                }
                worker_thread.swap(worker_thread_);
            } else {
                return task_status_;
            }
        }

        worker_thread.join();

        std::lock_guard<std::mutex> lock(mutex_);
        return task_status_;
    }

    SyncTaskState state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return task_state_;
    }

    Status status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return task_status_;
    }

    SyncTaskInfo info() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return task_info_;
    }

    bool is_running() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return task_state_ != SyncTaskState::kFinished;
    }

    static void serve_started_callback(
        const std::string& bound_host,
        int bound_port,
        void* user_context) {
        if (user_context == NULL) {
            return;
        }

        Impl* task_impl = reinterpret_cast<Impl*>(user_context);
        task_impl->set_local_endpoint(bound_host, bound_port);
    }

private:
    void run(const std::function<Status()>& worker_fn) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (task_state_ == SyncTaskState::kStarting) {
                task_state_ = SyncTaskState::kRunning;
            }
        }

        const Status task_status = worker_fn();
        mark_finished(task_status);
    }

    mutable std::mutex mutex_;
    SyncTaskState task_state_;
    Status task_status_;
    SyncTaskInfo task_info_;
    std::atomic<bool> stop_requested_;
    std::thread worker_thread_;
    std::function<void()> interrupt_callback_;
};

namespace {

std::unique_ptr<SyncTaskHandle> make_finished_sync_task_handle(
    SyncTaskMode task_mode,
    const std::string& device_id,
    const Status& task_status) {
    SyncTaskInfo task_info;
    task_info.mode = task_mode;
    task_info.device_id = device_id;

    std::shared_ptr<SyncTaskHandle::Impl> task_impl(new SyncTaskHandle::Impl(task_info));
    task_impl->mark_finished(task_status);
    return std::unique_ptr<SyncTaskHandle>(new SyncTaskHandle(task_impl));
}

}  // namespace

SyncTaskHandle::SyncTaskHandle() : impl_() {}

SyncTaskHandle::SyncTaskHandle(const std::shared_ptr<Impl>& impl) : impl_(impl) {}

SyncTaskHandle::~SyncTaskHandle() {}

SyncTaskHandle::SyncTaskHandle(SyncTaskHandle&& other) : impl_(std::move(other.impl_)) {}

SyncTaskHandle& SyncTaskHandle::operator=(SyncTaskHandle&& other) {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

void SyncTaskHandle::stop() {
    if (impl_) {
        impl_->request_stop();
    }
}

Status SyncTaskHandle::wait() {
    if (!impl_) {
        return Status(StatusCode::kSyncProtocolError, "Sync task handle is not initialized");
    }
    return impl_->wait();
}

SyncTaskState SyncTaskHandle::state() const {
    if (!impl_) {
        return SyncTaskState::kFinished;
    }
    return impl_->state();
}

Status SyncTaskHandle::last_status() const {
    if (!impl_) {
        return Status(StatusCode::kSyncProtocolError, "Sync task handle is not initialized");
    }
    return impl_->status();
}

SyncTaskInfo SyncTaskHandle::info() const {
    if (!impl_) {
        return SyncTaskInfo();
    }
    return impl_->info();
}

bool SyncTaskHandle::is_running() const {
    if (!impl_) {
        return false;
    }
    return impl_->is_running();
}

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

Manager::Manager()
    : data_manager_(),
      state_dir_(),
      local_device_id_(),
      local_device_name_(),
      trusted_store_path_(),
      initialized_(false) {}

Status Manager::create_from_root(const std::string& root_path, Manager* manager_out) {
    if (manager_out == NULL) {
        return Status(StatusCode::kSyncProtocolError, "manager output cannot be null");
    }

    return manager_out->initialize_from_root(root_path);
}

const std::string& Manager::root_path() const {
    return data_manager_.root_path;
}

const std::string& Manager::local_device_id() const {
    return local_device_id_;
}

const std::string& Manager::local_device_name() const {
    return local_device_name_;
}

const std::string& Manager::state_dir() const {
    return state_dir_;
}

Status Manager::initialize_from_root(const std::string& root_path) {
    const DataManager located_manager = DataManager::locate(root_path);
    if (!located_manager.is_valid()) {
        return located_manager.last_status();
    }

    const std::string state_dir = join_path(located_manager.root_path, kStateDirectoryName);
    const Status ensure_state_status = internal::ensure_directory_tree(state_dir, 0755);
    if (!ensure_state_status.ok()) {
        return ensure_state_status;
    }

    const std::string device_id_path = join_path(state_dir, kDeviceIdFileName);
    std::string local_device_id;
    const Status device_id_status = load_or_create_device_id(device_id_path, &local_device_id);
    if (!device_id_status.ok()) {
        return device_id_status;
    }

    std::string local_device_name;
    const Status device_name_status = internal::current_device_name(&local_device_name);
    if (!device_name_status.ok() || local_device_name.empty()) {
        local_device_name = local_device_id;
    }

    data_manager_ = located_manager;
    state_dir_ = state_dir;
    local_device_id_ = local_device_id;
    local_device_name_ = local_device_name;
    trusted_store_path_ = join_path(state_dir, kTrustedStoreFileName);
    initialized_ = true;

    return Status::ok_status();
}

Status Manager::ensure_initialized() const {
    if (!initialized_) {
        return Status(
            StatusCode::kSyncProtocolError,
            "Manager is not initialized. Call Manager::create_from_root first");
    }

    if (!data_manager_.is_valid()) {
        return data_manager_.last_status();
    }

    if (local_device_id_.empty()) {
        return Status(StatusCode::kSyncProtocolError, "local_device_id is empty");
    }
    if (local_device_name_.empty()) {
        return Status(StatusCode::kSyncProtocolError, "local_device_name is empty");
    }

    return Status::ok_status();
}

Status Manager::discover_devices(std::vector<DeviceInfo>* devices) const {
    if (devices == NULL) {
        return Status(StatusCode::kSyncProtocolError, "devices output cannot be null");
    }

    const Status initialized_status = ensure_initialized();
    if (!initialized_status.ok()) {
        return initialized_status;
    }

    TcpSyncTransport transport;
    MdnsDeviceDiscovery discovery;
    JsonTrustedDeviceStore trusted_store(trusted_store_path_);
    SyncManager sync_manager(
        data_manager_,
        local_device_id_,
        &transport,
        &discovery,
        &trusted_store);

    return sync_manager.discover_devices(devices);
}

Status Manager::resolve_pair_target(
    const PairSyncRequest& request,
    std::vector<DeviceInfo>* candidates,
    DeviceInfo* auto_selected) const {
    if (candidates == NULL || auto_selected == NULL) {
        return Status(
            StatusCode::kSyncProtocolError,
            "candidates and auto_selected outputs cannot be null");
    }

    candidates->clear();
    *auto_selected = DeviceInfo();

    const Status initialized_status = ensure_initialized();
    if (!initialized_status.ok()) {
        return initialized_status;
    }

    if (request.host.empty() && request.port > 0) {
        return Status(StatusCode::kSyncProtocolError, "--host is required when --port is provided");
    }

    if (!request.device_id.empty() && !request.host.empty()) {
        DeviceInfo selected_device;
        selected_device.device_id = request.device_id;
        selected_device.device_name = request.device_id;
        selected_device.host = request.host;
        selected_device.port = request.port > 0 ? request.port : kDefaultSyncPort;

        candidates->push_back(selected_device);
        *auto_selected = selected_device;
        return Status::ok_status();
    }

    std::vector<DeviceInfo> discovered_devices;
    const Status discover_status = discover_devices(&discovered_devices);
    if (!discover_status.ok()) {
        return discover_status;
    }

    if (request.device_id.empty()) {
        for (std::vector<DeviceInfo>::const_iterator it = discovered_devices.begin();
             it != discovered_devices.end();
             ++it) {
            if (!request.host.empty() && request.host != it->host) {
                continue;
            }

            DeviceInfo candidate = *it;
            if (request.port > 0) {
                candidate.port = request.port;
            }
            candidates->push_back(candidate);
        }

        if (candidates->empty()) {
            return Status(
                StatusCode::kDiscoveryError,
                "No discovered device matched pair request");
        }

        *auto_selected = candidates->front();
        return Status::ok_status();
    }

    for (std::vector<DeviceInfo>::const_iterator it = discovered_devices.begin();
         it != discovered_devices.end();
         ++it) {
        if (it->device_id != request.device_id) {
            continue;
        }

        DeviceInfo selected_device = *it;
        if (!request.host.empty()) {
            selected_device.host = request.host;
        }
        if (request.port > 0) {
            selected_device.port = request.port;
        }
        if (selected_device.port <= 0) {
            selected_device.port = kDefaultSyncPort;
        }

        candidates->push_back(selected_device);
        *auto_selected = selected_device;
        return Status::ok_status();
    }

    return Status(
        StatusCode::kDiscoveryError,
        "Device not found via discovery. Set --host/--port explicitly.");
}

std::unique_ptr<SyncTaskHandle> Manager::serve_sync(const ServeSyncOptions& options) const {
    const Status initialized_status = ensure_initialized();
    if (!initialized_status.ok()) {
        return make_finished_sync_task_handle(
            SyncTaskMode::kServe,
            local_device_id_,
            initialized_status);
    }

    SyncTaskInfo task_info;
    task_info.mode = SyncTaskMode::kServe;
    task_info.device_id = local_device_id_;
    task_info.local_ip = options.server_options.bind_host;
    task_info.local_port = options.server_options.port;

    std::shared_ptr<SyncTaskHandle::Impl> task_impl(new SyncTaskHandle::Impl(task_info));
    std::unique_ptr<SyncTaskHandle> task_handle(new SyncTaskHandle(task_impl));
    SyncTaskHandle::Impl* task_impl_ptr = task_impl.get();

    ServerOptions server_options = options.server_options;
    if (server_options.advertise_name.empty()) {
        server_options.advertise_name = local_device_name_;
    }

    const DataManager data_manager = data_manager_;
    const std::string local_device_id = local_device_id_;
    const std::string trusted_store_path = trusted_store_path_;

    task_impl->start([task_impl_ptr, server_options, data_manager, local_device_id, trusted_store_path]() -> Status {
        JsonTrustedDeviceStore trusted_store(trusted_store_path);

        int bound_port = 0;
        std::string bound_host;
        const Status run_status = serve_sync_server(
            data_manager,
            local_device_id,
            &trusted_store,
            server_options,
            &bound_port,
            &bound_host,
            task_impl_ptr->stop_requested_ptr(),
            &SyncTaskHandle::Impl::serve_started_callback,
            task_impl_ptr);
        if (!run_status.ok()) {
            return run_status;
        }

        if (task_impl_ptr->stop_requested()) {
            return Status::ok_status();
        }
        return Status::ok_status();
    });

    return task_handle;
}

std::unique_ptr<SyncTaskHandle> Manager::pair_sync(
    const DeviceInfo& device_info,
    const PairSyncOptions& options) const {
    const Status initialized_status = ensure_initialized();
    if (!initialized_status.ok()) {
        return make_finished_sync_task_handle(
            SyncTaskMode::kPair,
            local_device_id_,
            initialized_status);
    }

    if (device_info.device_id.empty()) {
        return make_finished_sync_task_handle(
            SyncTaskMode::kPair,
            local_device_id_,
            Status(StatusCode::kSyncProtocolError, "device_id cannot be empty"));
    }
    if (!is_connectable_host(device_info.host) || device_info.port <= 0) {
        return make_finished_sync_task_handle(
            SyncTaskMode::kPair,
            local_device_id_,
            Status(StatusCode::kSyncProtocolError, "remote device host/port is not connectable"));
    }

    SyncTaskInfo task_info;
    task_info.mode = SyncTaskMode::kPair;
    task_info.device_id = local_device_id_;

    std::shared_ptr<SyncTaskHandle::Impl> task_impl(new SyncTaskHandle::Impl(task_info));
    std::unique_ptr<SyncTaskHandle> task_handle(new SyncTaskHandle(task_impl));
    SyncTaskHandle::Impl* task_impl_ptr = task_impl.get();

    const DataManager data_manager = data_manager_;
    const std::string local_device_id = local_device_id_;
    const std::string trusted_store_path = trusted_store_path_;

    task_impl->start([task_impl_ptr, device_info, options, data_manager, local_device_id, trusted_store_path]() -> Status {
        if (task_impl_ptr->stop_requested()) {
            return make_sync_canceled_status();
        }

        std::shared_ptr<TcpSyncTransport> transport(new TcpSyncTransport());
        task_impl_ptr->set_interrupt_callback([transport]() {
            transport->disconnect();
        });

        MdnsDeviceDiscovery discovery;
        JsonTrustedDeviceStore trusted_store(trusted_store_path);
        SyncManager sync_manager(
            data_manager,
            local_device_id,
            transport.get(),
            &discovery,
            &trusted_store);

        const Status load_status = trusted_store.load();
        if (!load_status.ok()) {
            return load_status;
        }
        if (task_impl_ptr->stop_requested()) {
            return make_sync_canceled_status();
        }

        if (!trusted_store.is_trusted(device_info.device_id)) {
            const Status pair_status = sync_manager.pair_device(device_info, options.pairing_options);
            if (!pair_status.ok()) {
                if (task_impl_ptr->stop_requested()) {
                    return make_sync_canceled_status();
                }
                return pair_status;
            }
        }
        if (task_impl_ptr->stop_requested()) {
            return make_sync_canceled_status();
        }

        const Status pull_status = sync_manager.pull_from_device(device_info, options.sync_options);
        if (!pull_status.ok()) {
            if (task_impl_ptr->stop_requested()) {
                return make_sync_canceled_status();
            }
            return pull_status;
        }

        std::string local_host;
        int local_port = 0;
        const Status endpoint_status = transport->local_endpoint(&local_host, &local_port);
        if (endpoint_status.ok()) {
            task_impl_ptr->set_local_endpoint(local_host, local_port);
        }

        if (task_impl_ptr->stop_requested()) {
            return make_sync_canceled_status();
        }
        return Status::ok_status();
    });

    return task_handle;
}

Status Manager::export_backup(const ExportBackupOptions& options, ExportBackupResult* result) const {
    if (result == NULL) {
        return Status(StatusCode::kSyncProtocolError, "result output cannot be null");
    }

    const Status initialized_status = ensure_initialized();
    if (!initialized_status.ok()) {
        return initialized_status;
    }

    if (options.file_path.empty()) {
        return Status(StatusCode::kSyncProtocolError, "backup export file_path cannot be empty");
    }

    std::ofstream out_file(options.file_path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out_file.is_open()) {
        return Status(StatusCode::kIoError, "Failed opening backup export file for write");
    }

    const Status backup_status = data_manager_.backup(out_file, options.backup_options);
    if (!backup_status.ok()) {
        return backup_status;
    }

    out_file.flush();
    if (!out_file) {
        return Status(StatusCode::kIoError, "Failed writing backup export file");
    }

    const std::streampos end_position = out_file.tellp();
    if (end_position < 0) {
        return Status(StatusCode::kIoError, "Failed determining backup export size");
    }

    result->file_path = options.file_path;
    result->bytes_written = static_cast<uint64_t>(end_position);
    return Status::ok_status();
}

Status Manager::restore_backup(const RestoreBackupOptions& options) const {
    const Status initialized_status = ensure_initialized();
    if (!initialized_status.ok()) {
        return initialized_status;
    }

    if (options.file_path.empty()) {
        return Status(StatusCode::kSyncProtocolError, "backup restore file_path cannot be empty");
    }

    std::ifstream in_file(options.file_path.c_str(), std::ios::binary);
    if (!in_file.is_open()) {
        return Status(StatusCode::kIoError, "Failed opening backup file for read");
    }

    return data_manager_.restore(in_file, data_manager_.root_path);
}

}  // namespace STManager
