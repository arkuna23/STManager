#include <STManager/data.h>
#include <STManager/manager.h>
#include <STManager/sync.h>
#include <archive.h>
#include <archive_entry.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../src/archive_stream.h"
#include "test_helpers.h"

using STManager::BackupOptions;
using STManager::DataManager;
using STManager::DeviceInfo;
using STManager::ExportBackupOptions;
using STManager::ExportBackupResult;
using STManager::IDeviceDiscovery;
using STManager::ISyncTransport;
using STManager::ITrustedDeviceStore;
using STManager::Manager;
using STManager::PairingOptions;
using STManager::PairSyncOptions;
using STManager::PairSyncRequest;
using STManager::RestoreBackupOptions;
using STManager::Status;
using STManager::StatusCode;
using STManager::SyncDirection;
using STManager::SyncManager;
using STManager::SyncOptions;

namespace {

struct TestContext {
    int failed_assertions;

    TestContext() : failed_assertions(0) {}
};

#define EXPECT_TRUE(ctx, expr)                                                               \
    do {                                                                                     \
        if (!(expr)) {                                                                       \
            std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__ << ": " #expr \
                      << "\n";                                                               \
            ++(ctx).failed_assertions;                                                       \
        }                                                                                    \
    } while (0)

#define EXPECT_EQ(ctx, lhs, rhs)                                                                   \
    do {                                                                                           \
        const auto _lhs_value = (lhs);                                                             \
        const auto _rhs_value = (rhs);                                                             \
        if (!(_lhs_value == _rhs_value)) {                                                         \
            std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__ << ": " #lhs        \
                      << " == " #rhs << " (actual: " << _lhs_value << ", expected: " << _rhs_value \
                      << ")\n";                                                                    \
            ++(ctx).failed_assertions;                                                             \
        }                                                                                          \
    } while (0)

#define EXPECT_STATUS_OK(ctx, status_expr)                                           \
    do {                                                                             \
        const Status _status_value = (status_expr);                                  \
        if (!_status_value.ok()) {                                                   \
            std::cerr << "Status failed at " << __FILE__ << ":" << __LINE__ << " : " \
                      << _status_value.message << "\n";                              \
            ++(ctx).failed_assertions;                                               \
        }                                                                            \
    } while (0)

struct TempDirGuard {
    std::string path;

    explicit TempDirGuard(const std::string& path_in) : path(path_in) {}

    ~TempDirGuard() {
        if (!path.empty()) {
            STManagerTest::remove_directory_recursive(path);
        }
    }
};

class FakeTransport : public ISyncTransport {
public:
    bool connected;
    std::vector<std::string> sent_messages;
    std::vector<std::string> queued_receive_messages;
    std::string sent_stream_data;
    std::string incoming_stream_data;

    FakeTransport()
        : connected(false),
          sent_messages(),
          queued_receive_messages(),
          sent_stream_data(),
          incoming_stream_data() {}

    Status connect(const DeviceInfo&) override {
        connected = true;
        return Status::ok_status();
    }

    Status disconnect() override {
        connected = false;
        return Status::ok_status();
    }

    Status send_message(const std::string& message) override {
        sent_messages.push_back(message);
        return Status::ok_status();
    }

    Status receive_message(std::string* message) override {
        if (queued_receive_messages.empty()) {
            return Status(StatusCode::kSyncProtocolError, "No queued fake receive message");
        }

        *message = queued_receive_messages.front();
        queued_receive_messages.erase(queued_receive_messages.begin());
        return Status::ok_status();
    }

    Status send_stream(std::istream& in) override {
        sent_stream_data.clear();
        char buffer[4096];
        while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0) {
            sent_stream_data.append(buffer, static_cast<std::string::size_type>(in.gcount()));
        }
        return Status::ok_status();
    }

    Status receive_stream(std::ostream& out) override {
        out.write(incoming_stream_data.data(),
                  static_cast<std::streamsize>(incoming_stream_data.size()));
        if (!out) {
            return Status(StatusCode::kIoError, "Failed writing fake incoming stream");
        }
        return Status::ok_status();
    }
};

class FakeDiscovery : public IDeviceDiscovery {
public:
    mutable bool start_called;
    mutable bool list_called;
    std::vector<DeviceInfo> devices;

    FakeDiscovery() : start_called(false), list_called(false), devices() {}

    Status start() override {
        start_called = true;
        return Status::ok_status();
    }

    Status stop() override { return Status::ok_status(); }

    Status list_devices(std::vector<DeviceInfo>* out_devices) const override {
        list_called = true;
        if (!start_called) {
            return Status(StatusCode::kDiscoveryError, "Discovery was not started");
        }

        *out_devices = devices;
        return Status::ok_status();
    }
};

class FakeTrustedStore : public ITrustedDeviceStore {
public:
    mutable bool load_called;
    mutable bool save_called;
    std::set<std::string> trusted_device_ids;

    FakeTrustedStore() : load_called(false), save_called(false), trusted_device_ids() {}

    Status load() override {
        load_called = true;
        return Status::ok_status();
    }

    Status save() const override {
        save_called = true;
        return Status::ok_status();
    }

    bool is_trusted(const std::string& device_id) const override {
        return trusted_device_ids.count(device_id) > 0;
    }

    Status trust_device(const std::string& device_id) override {
        trusted_device_ids.insert(device_id);
        return Status::ok_status();
    }

    Status untrust_device(const std::string& device_id) override {
        trusted_device_ids.erase(device_id);
        return Status::ok_status();
    }
};

std::string create_malicious_archive_file() {
    const std::string temp_dir =
        STManagerTest::create_temp_directory("stmanager-malicious-archive");
    if (temp_dir.empty()) {
        return std::string();
    }

    const std::string archive_path = STManagerTest::join_path(temp_dir, "malicious.tar.zst");

    struct archive* archive_writer = archive_write_new();
    if (archive_writer == NULL) {
        STManagerTest::remove_directory_recursive(temp_dir);
        return std::string();
    }

    archive_write_set_format_pax_restricted(archive_writer);
    if (archive_write_add_filter_zstd(archive_writer) != ARCHIVE_OK) {
        archive_write_free(archive_writer);
        STManagerTest::remove_directory_recursive(temp_dir);
        return std::string();
    }

    if (archive_write_open_filename(archive_writer, archive_path.c_str()) != ARCHIVE_OK) {
        archive_write_free(archive_writer);
        STManagerTest::remove_directory_recursive(temp_dir);
        return std::string();
    }

    const std::string payload = "evil";
    struct archive_entry* entry = archive_entry_new();
    archive_entry_set_pathname(entry, "../evil.txt");
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_size(entry, static_cast<la_int64_t>(payload.size()));

    if (archive_write_header(archive_writer, entry) == ARCHIVE_OK) {
        archive_write_data(archive_writer, payload.data(), payload.size());
    }

    archive_entry_free(entry);
    archive_write_close(archive_writer);
    archive_write_free(archive_writer);

    std::string archive_bytes = STManagerTest::read_file(archive_path);
    STManagerTest::remove_directory_recursive(temp_dir);
    return archive_bytes;
}

struct ArchiveFileEntry {
    std::string path;
    std::string content;
};

std::vector<std::string> split_path_segments(const std::string& path) {
    std::vector<std::string> segments;
    std::string current;
    for (std::string::size_type index = 0; index < path.size(); ++index) {
        const char value = path[index];
        if (value == '/') {
            if (!current.empty()) {
                segments.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(value);
    }

    if (!current.empty()) {
        segments.push_back(current);
    }
    return segments;
}

std::string create_valid_archive_bytes(const std::vector<ArchiveFileEntry>& file_entries) {
    const std::string temp_dir = STManagerTest::create_temp_directory("stmanager-valid-archive");
    if (temp_dir.empty()) {
        return std::string();
    }

    const std::string archive_path = STManagerTest::join_path(temp_dir, "valid.tar.zst");

    struct archive* archive_writer = archive_write_new();
    if (archive_writer == NULL) {
        STManagerTest::remove_directory_recursive(temp_dir);
        return std::string();
    }

    archive_write_set_format_pax_restricted(archive_writer);
    if (archive_write_add_filter_zstd(archive_writer) != ARCHIVE_OK) {
        archive_write_free(archive_writer);
        STManagerTest::remove_directory_recursive(temp_dir);
        return std::string();
    }

    if (archive_write_open_filename(archive_writer, archive_path.c_str()) != ARCHIVE_OK) {
        archive_write_free(archive_writer);
        STManagerTest::remove_directory_recursive(temp_dir);
        return std::string();
    }

    std::set<std::string> created_directories;
    for (std::vector<ArchiveFileEntry>::const_iterator it = file_entries.begin();
         it != file_entries.end(); ++it) {
        const std::vector<std::string> segments = split_path_segments(it->path);
        std::string current_directory;
        for (std::vector<std::string>::size_type index = 0; index + 1 < segments.size(); ++index) {
            if (current_directory.empty()) {
                current_directory = segments[index];
            } else {
                current_directory += "/" + segments[index];
            }

            if (created_directories.count(current_directory) > 0) {
                continue;
            }

            struct archive_entry* directory_entry = archive_entry_new();
            archive_entry_set_pathname(directory_entry, current_directory.c_str());
            archive_entry_set_filetype(directory_entry, AE_IFDIR);
            archive_entry_set_perm(directory_entry, 0755);
            archive_entry_set_size(directory_entry, 0);
            archive_write_header(archive_writer, directory_entry);
            archive_entry_free(directory_entry);
            created_directories.insert(current_directory);
        }

        struct archive_entry* file_entry = archive_entry_new();
        archive_entry_set_pathname(file_entry, it->path.c_str());
        archive_entry_set_filetype(file_entry, AE_IFREG);
        archive_entry_set_perm(file_entry, 0644);
        archive_entry_set_size(file_entry, static_cast<la_int64_t>(it->content.size()));
        if (archive_write_header(archive_writer, file_entry) == ARCHIVE_OK) {
            archive_write_data(archive_writer, it->content.data(), it->content.size());
        }
        archive_entry_free(file_entry);
    }

    archive_write_close(archive_writer);
    archive_write_free(archive_writer);

    const std::string archive_bytes = STManagerTest::read_file(archive_path);
    STManagerTest::remove_directory_recursive(temp_dir);
    return archive_bytes;
}

bool test_locate_success_with_required_dirs() {
    TestContext context;

    const std::string fixture_root = STManagerTest::create_sillytavern_fixture("locate-success");
    TempDirGuard fixture_guard(fixture_root);

    EXPECT_TRUE(context, !fixture_root.empty());

    const DataManager manager = DataManager::locate(fixture_root);
    EXPECT_TRUE(context, manager.is_valid());
    EXPECT_EQ(context, manager.root_path, fixture_root);
    EXPECT_EQ(context, manager.extensions_path,
              STManagerTest::join_path(fixture_root, "public/scripts/extensions"));
    EXPECT_EQ(context, manager.data_path, STManagerTest::join_path(fixture_root, "data"));

    return context.failed_assertions == 0;
}

bool test_locate_missing_extensions_fails() {
    TestContext context;

    const std::string fixture_root =
        STManagerTest::create_sillytavern_fixture("locate-missing-ext");
    TempDirGuard fixture_guard(fixture_root);

    EXPECT_TRUE(context, STManagerTest::remove_directory_recursive(
                             STManagerTest::join_path(fixture_root, "public/scripts/extensions")));

    const DataManager manager = DataManager::locate(fixture_root);
    EXPECT_TRUE(context, !manager.is_valid());
    EXPECT_EQ(context, static_cast<int>(manager.last_status().code),
              static_cast<int>(StatusCode::kMissingExtensionsDir));

    return context.failed_assertions == 0;
}

bool test_locate_missing_data_fails() {
    TestContext context;

    const std::string fixture_root =
        STManagerTest::create_sillytavern_fixture("locate-missing-data");
    TempDirGuard fixture_guard(fixture_root);

    EXPECT_TRUE(context, STManagerTest::remove_directory_recursive(
                             STManagerTest::join_path(fixture_root, "data")));

    const DataManager manager = DataManager::locate(fixture_root);
    EXPECT_TRUE(context, !manager.is_valid());
    EXPECT_EQ(context, static_cast<int>(manager.last_status().code),
              static_cast<int>(StatusCode::kMissingDataDir));

    return context.failed_assertions == 0;
}

bool test_backup_restore_roundtrip_data_and_extensions() {
    TestContext context;

    const std::string source_root =
        STManagerTest::create_sillytavern_fixture("backup-roundtrip-src");
    const std::string restore_root =
        STManagerTest::create_sillytavern_fixture("backup-roundtrip-dst");
    TempDirGuard source_guard(source_root);
    TempDirGuard restore_guard(restore_root);

    EXPECT_TRUE(context, !source_root.empty());
    EXPECT_TRUE(context, !restore_root.empty());

    EXPECT_TRUE(context,
                STManagerTest::write_file(STManagerTest::join_path(source_root, "data/config.json"),
                                          "{\"name\":\"demo\"}"));
    EXPECT_TRUE(context, STManagerTest::write_file(
                             STManagerTest::join_path(source_root,
                                                      "public/scripts/extensions/ext-a/index.js"),
                             "module.exports={};"));

    const DataManager source_manager = DataManager::locate(source_root);
    EXPECT_TRUE(context, source_manager.is_valid());

    const std::string backup_archive_path =
        STManagerTest::join_path(source_root, "roundtrip.tar.zst");
    {
        std::ofstream archive_out(backup_archive_path.c_str(), std::ios::binary | std::ios::trunc);
        EXPECT_TRUE(context, archive_out.is_open());
        EXPECT_STATUS_OK(context, source_manager.backup(archive_out));
    }
    EXPECT_TRUE(context, STManagerTest::read_file(backup_archive_path).size() > 0);

    const std::string restore_archive_bytes =
        create_valid_archive_bytes(std::vector<ArchiveFileEntry>{
            {"data/config.json", "{\"name\":\"demo\"}"},
            {"extensions/ext-a/index.js", "module.exports={};"},
        });
    EXPECT_TRUE(context, !restore_archive_bytes.empty());

    std::stringstream restore_stream(std::ios::in | std::ios::out | std::ios::binary);
    restore_stream.write(restore_archive_bytes.data(),
                         static_cast<std::streamsize>(restore_archive_bytes.size()));
    restore_stream.clear();
    restore_stream.seekg(0, std::ios::beg);
    EXPECT_STATUS_OK(context, source_manager.restore(restore_stream, restore_root));

    EXPECT_EQ(context,
              STManagerTest::read_file(STManagerTest::join_path(restore_root, "data/config.json")),
              "{\"name\":\"demo\"}");
    EXPECT_EQ(context,
              STManagerTest::read_file(STManagerTest::join_path(
                  restore_root, "public/scripts/extensions/ext-a/index.js")),
              "module.exports={};");

    return context.failed_assertions == 0;
}

bool test_backup_git_mode_skips_git_extensions_and_writes_manifest() {
    TestContext context;

    const std::string source_root = STManagerTest::create_sillytavern_fixture("backup-git-mode");
    const std::string restore_root = STManagerTest::create_temp_directory("backup-git-mode-dst");
    TempDirGuard source_guard(source_root);
    TempDirGuard restore_guard(restore_root);

    EXPECT_TRUE(context, STManagerTest::write_file(
                             STManagerTest::join_path(
                                 source_root, "public/scripts/extensions/ext-git/.git/config"),
                             "[remote \"origin\"]\n    url = https://example.com/ext-git.git\n"));
    EXPECT_TRUE(context, STManagerTest::write_file(
                             STManagerTest::join_path(source_root,
                                                      "public/scripts/extensions/ext-git/main.js"),
                             "git content"));
    EXPECT_TRUE(context, STManagerTest::write_file(
                             STManagerTest::join_path(
                                 source_root, "public/scripts/extensions/ext-normal/main.js"),
                             "normal content"));

    const DataManager manager = DataManager::locate(source_root);
    EXPECT_TRUE(context, manager.is_valid());

    BackupOptions options;
    options.git_mode_for_extensions = true;

    const std::string archive_path = STManagerTest::join_path(source_root, "git-mode.tar.zst");
    {
        std::ofstream archive_out(archive_path.c_str(), std::ios::binary | std::ios::trunc);
        EXPECT_TRUE(context, archive_out.is_open());
        EXPECT_STATUS_OK(context, manager.backup(archive_out, options));
    }
    const std::string archive_bytes = STManagerTest::read_file(archive_path);
    EXPECT_TRUE(context, !archive_bytes.empty());

    return context.failed_assertions == 0;
}

bool test_backup_archive_stream_validates_successfully() {
    TestContext context;

    const std::string source_root = STManagerTest::create_sillytavern_fixture("backup-validate");
    TempDirGuard source_guard(source_root);

    EXPECT_TRUE(context, STManagerTest::write_file(
                             STManagerTest::join_path(source_root, "data/worlds/settings.json"),
                             "{\"mode\":\"single\"}"));
    EXPECT_TRUE(context, STManagerTest::write_file(
                             STManagerTest::join_path(
                                 source_root, "public/scripts/extensions/ext-validate/main.js"),
                             "module.exports = true;"));

    const DataManager manager = DataManager::locate(source_root);
    EXPECT_TRUE(context, manager.is_valid());

    std::stringstream backup_stream(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_STATUS_OK(context, manager.backup(backup_stream));

    backup_stream.clear();
    backup_stream.seekg(0, std::ios::beg);
    EXPECT_STATUS_OK(context, STManager::internal::validate_backup_archive(backup_stream));

    return context.failed_assertions == 0;
}

bool test_backup_archive_with_nested_directories_validates_successfully() {
    TestContext context;

    const std::string source_root =
        STManagerTest::create_sillytavern_fixture("backup-validate-nested");
    TempDirGuard source_guard(source_root);

    EXPECT_TRUE(context, STManagerTest::create_directories(STManagerTest::join_path(
                             source_root, "data/worlds/campaign/chapter1")));
    EXPECT_TRUE(context, STManagerTest::create_directories(STManagerTest::join_path(
                             source_root, "public/scripts/extensions/ext-deep/assets/icons")));
    EXPECT_TRUE(context, STManagerTest::write_file(
                             STManagerTest::join_path(source_root,
                                                      "data/worlds/campaign/chapter1/state.json"),
                             "{\"chapter\":1}"));
    EXPECT_TRUE(context,
                STManagerTest::write_file(
                    STManagerTest::join_path(
                        source_root, "public/scripts/extensions/ext-deep/assets/icons/readme.txt"),
                    "nested asset"));

    const DataManager manager = DataManager::locate(source_root);
    EXPECT_TRUE(context, manager.is_valid());

    std::stringstream backup_stream(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_STATUS_OK(context, manager.backup(backup_stream));

    backup_stream.clear();
    backup_stream.seekg(0, std::ios::beg);
    EXPECT_STATUS_OK(context, STManager::internal::validate_backup_archive(backup_stream));

    return context.failed_assertions == 0;
}

bool test_restore_rejects_traversal_entry() {
    TestContext context;

    const std::string source_root =
        STManagerTest::create_sillytavern_fixture("restore-traversal-src");
    const std::string restore_root = STManagerTest::create_temp_directory("restore-traversal-dst");
    TempDirGuard source_guard(source_root);
    TempDirGuard restore_guard(restore_root);

    const std::string archive_bytes = create_malicious_archive_file();
    EXPECT_TRUE(context, !archive_bytes.empty());

    const DataManager manager = DataManager::locate(source_root);
    EXPECT_TRUE(context, manager.is_valid());

    std::stringstream archive_stream(std::ios::in | std::ios::out | std::ios::binary);
    archive_stream.write(archive_bytes.data(), static_cast<std::streamsize>(archive_bytes.size()));
    archive_stream.clear();
    archive_stream.seekg(0, std::ios::beg);

    const Status restore_status = manager.restore(archive_stream, restore_root);
    EXPECT_TRUE(context, !restore_status.ok());
    EXPECT_EQ(context, static_cast<int>(restore_status.code),
              static_cast<int>(StatusCode::kInvalidArchiveEntry));

    return context.failed_assertions == 0;
}

bool test_restore_full_replace_removes_stale_files() {
    TestContext context;

    const std::string source_root =
        STManagerTest::create_sillytavern_fixture("restore-full-replace-src");
    const std::string restore_root =
        STManagerTest::create_sillytavern_fixture("restore-full-replace-dst");
    TempDirGuard source_guard(source_root);
    TempDirGuard restore_guard(restore_root);

    EXPECT_TRUE(context,
                STManagerTest::write_file(STManagerTest::join_path(restore_root, "data/stale.json"),
                                          "{\"stale\":true}"));
    EXPECT_TRUE(context, STManagerTest::write_file(
                             STManagerTest::join_path(
                                 restore_root, "public/scripts/extensions/stale-extension/main.js"),
                             "stale"));

    const std::string archive_bytes = create_valid_archive_bytes(std::vector<ArchiveFileEntry>{
        {"data/state.json", "{\"fresh\":true}"},
        {"extensions/ext-a/index.js", "new extension"},
    });
    EXPECT_TRUE(context, !archive_bytes.empty());

    const DataManager manager = DataManager::locate(source_root);
    EXPECT_TRUE(context, manager.is_valid());

    std::stringstream archive_stream(std::ios::in | std::ios::out | std::ios::binary);
    archive_stream.write(archive_bytes.data(), static_cast<std::streamsize>(archive_bytes.size()));
    archive_stream.clear();
    archive_stream.seekg(0, std::ios::beg);
    EXPECT_STATUS_OK(context, manager.restore(archive_stream, restore_root));

    EXPECT_EQ(context,
              STManagerTest::read_file(STManagerTest::join_path(restore_root, "data/state.json")),
              "{\"fresh\":true}");
    EXPECT_EQ(context,
              STManagerTest::read_file(STManagerTest::join_path(
                  restore_root, "public/scripts/extensions/ext-a/index.js")),
              "new extension");
    EXPECT_TRUE(context, !STManagerTest::path_exists(
                             STManagerTest::join_path(restore_root, "data/stale.json")));
    EXPECT_TRUE(context, !STManagerTest::path_exists(STManagerTest::join_path(
                             restore_root, "public/scripts/extensions/stale-extension/main.js")));

    return context.failed_assertions == 0;
}

bool test_restore_to_sillytavern_layout_replaces_public_extensions() {
    TestContext context;

    const std::string source_root = STManagerTest::create_sillytavern_fixture("restore-layout-src");
    const std::string restore_root =
        STManagerTest::create_sillytavern_fixture("restore-layout-dst");
    TempDirGuard source_guard(source_root);
    TempDirGuard restore_guard(restore_root);

    EXPECT_TRUE(context,
                STManagerTest::write_file(STManagerTest::join_path(restore_root, "data/old.json"),
                                          "{\"old\":true}"));
    EXPECT_TRUE(context, STManagerTest::write_file(
                             STManagerTest::join_path(restore_root,
                                                      "public/scripts/extensions/legacy/main.js"),
                             "legacy"));

    const std::string archive_bytes = create_valid_archive_bytes(std::vector<ArchiveFileEntry>{
        {"data/new.json", "{\"new\":true}"},
        {"extensions/ext-fresh/main.js", "fresh extension"},
    });
    EXPECT_TRUE(context, !archive_bytes.empty());

    const DataManager manager = DataManager::locate(source_root);
    EXPECT_TRUE(context, manager.is_valid());

    std::stringstream archive_stream(std::ios::in | std::ios::out | std::ios::binary);
    archive_stream.write(archive_bytes.data(), static_cast<std::streamsize>(archive_bytes.size()));
    archive_stream.clear();
    archive_stream.seekg(0, std::ios::beg);
    EXPECT_STATUS_OK(context, manager.restore(archive_stream, restore_root));

    EXPECT_EQ(context,
              STManagerTest::read_file(STManagerTest::join_path(restore_root, "data/new.json")),
              "{\"new\":true}");
    EXPECT_EQ(context,
              STManagerTest::read_file(STManagerTest::join_path(
                  restore_root, "public/scripts/extensions/ext-fresh/main.js")),
              "fresh extension");

    EXPECT_TRUE(context, !STManagerTest::path_exists(
                             STManagerTest::join_path(restore_root, "data/old.json")));
    EXPECT_TRUE(context, !STManagerTest::path_exists(STManagerTest::join_path(
                             restore_root, "public/scripts/extensions/legacy/main.js")));
    EXPECT_TRUE(context, !STManagerTest::path_exists(STManagerTest::join_path(
                             restore_root, "extensions/ext-fresh/main.js")));

    return context.failed_assertions == 0;
}

bool test_sync_push_requires_trusted_device() {
    TestContext context;

    const std::string root = STManagerTest::create_sillytavern_fixture("sync-push-untrusted");
    TempDirGuard root_guard(root);

    EXPECT_TRUE(context,
                STManagerTest::write_file(STManagerTest::join_path(root, "data/state.json"), "{}"));

    const DataManager manager = DataManager::locate(root);
    FakeTransport transport;
    FakeDiscovery discovery;
    FakeTrustedStore trust_store;

    SyncManager sync_manager(manager, "local-device", &transport, &discovery, &trust_store);

    DeviceInfo remote_device;
    remote_device.device_id = "device-a";
    remote_device.device_name = "Device A";
    remote_device.host = "127.0.0.1";
    remote_device.port = 12345;

    const SyncOptions options;
    const Status push_status = sync_manager.push_to_device(remote_device, options);
    EXPECT_TRUE(context, !push_status.ok());
    EXPECT_EQ(context, static_cast<int>(push_status.code),
              static_cast<int>(StatusCode::kUnauthorized));

    return context.failed_assertions == 0;
}

bool test_sync_pair_then_push_success() {
    TestContext context;

    const std::string root = STManagerTest::create_sillytavern_fixture("sync-pair-push");
    TempDirGuard root_guard(root);

    EXPECT_TRUE(context, STManagerTest::write_file(
                             STManagerTest::join_path(root, "data/state.json"), "{\"sync\":true}"));

    const DataManager manager = DataManager::locate(root);
    FakeTransport transport;
    FakeDiscovery discovery;
    FakeTrustedStore trust_store;

    transport.queued_receive_messages.push_back("{\"type\":\"pair_response\",\"ok\":true}");
    transport.queued_receive_messages.push_back("{\"type\":\"auth_response\",\"ok\":true}");

    SyncManager sync_manager(manager, "local-device", &transport, &discovery, &trust_store);

    DeviceInfo remote_device;
    remote_device.device_id = "device-b";
    remote_device.device_name = "Device B";
    remote_device.host = "127.0.0.1";
    remote_device.port = 12444;

    PairingOptions pairing_options;
    pairing_options.pairing_code = "123456";
    EXPECT_STATUS_OK(context, sync_manager.pair_device(remote_device, pairing_options));

    const SyncOptions sync_options;
    EXPECT_STATUS_OK(context, sync_manager.push_to_device(remote_device, sync_options));

    EXPECT_TRUE(context, trust_store.is_trusted(remote_device.device_id));
    EXPECT_TRUE(context, trust_store.save_called);
    EXPECT_TRUE(context, transport.sent_stream_data.size() > 0);
    EXPECT_TRUE(context, transport.sent_messages.size() >= 2);

    return context.failed_assertions == 0;
}

bool test_sync_pull_restores_to_override_root() {
    TestContext context;

    const std::string source_root = STManagerTest::create_sillytavern_fixture("sync-pull-src");
    const std::string local_root = STManagerTest::create_sillytavern_fixture("sync-pull-local");
    const std::string override_root =
        STManagerTest::create_sillytavern_fixture("sync-pull-override");
    TempDirGuard source_guard(source_root);
    TempDirGuard local_guard(local_root);
    TempDirGuard override_guard(override_root);

    EXPECT_TRUE(context,
                STManagerTest::write_file(
                    STManagerTest::join_path(source_root, "data/history.json"), "pull payload"));

    const std::string backup_bytes = create_valid_archive_bytes(std::vector<ArchiveFileEntry>{
        {"data/history.json", "pull payload"},
        {"extensions/ext-pull/main.js", "pull extension"},
    });
    EXPECT_TRUE(context, !backup_bytes.empty());

    const DataManager local_manager = DataManager::locate(local_root);
    FakeTransport transport;
    FakeDiscovery discovery;
    FakeTrustedStore trust_store;
    trust_store.trusted_device_ids.insert("device-c");

    transport.queued_receive_messages.push_back("{\"type\":\"auth_response\",\"ok\":true}");
    transport.incoming_stream_data = backup_bytes;

    SyncManager sync_manager(local_manager, "local-device", &transport, &discovery, &trust_store);

    DeviceInfo remote_device;
    remote_device.device_id = "device-c";
    remote_device.device_name = "Device C";
    remote_device.host = "127.0.0.1";
    remote_device.port = 12555;

    SyncOptions options;
    options.destination_root_override = override_root;

    EXPECT_STATUS_OK(context, sync_manager.pull_from_device(remote_device, options));
    EXPECT_EQ(
        context,
        STManagerTest::read_file(STManagerTest::join_path(override_root, "data/history.json")),
        "pull payload");
    EXPECT_EQ(context,
              STManagerTest::read_file(STManagerTest::join_path(
                  override_root, "public/scripts/extensions/ext-pull/main.js")),
              "pull extension");

    return context.failed_assertions == 0;
}

bool test_discover_devices_autostarts_discovery() {
    TestContext context;

    const std::string root = STManagerTest::create_sillytavern_fixture("discover-devices");
    TempDirGuard root_guard(root);

    const DataManager manager = DataManager::locate(root);
    FakeTransport transport;
    FakeDiscovery discovery;
    FakeTrustedStore trust_store;

    DeviceInfo device;
    device.device_id = "device-d";
    device.device_name = "Device D";
    device.host = "10.0.0.2";
    device.port = 13131;
    discovery.devices.push_back(device);

    SyncManager sync_manager(manager, "local-device", &transport, &discovery, &trust_store);

    std::vector<DeviceInfo> discovered_devices;
    EXPECT_STATUS_OK(context, sync_manager.discover_devices(&discovered_devices));
    EXPECT_TRUE(context, discovery.start_called);
    EXPECT_TRUE(context, discovery.list_called);
    EXPECT_EQ(context, discovered_devices.size(), static_cast<size_t>(1));
    EXPECT_EQ(context, discovered_devices[0].device_id, std::string("device-d"));

    return context.failed_assertions == 0;
}

bool test_manager_create_from_root_creates_state() {
    TestContext context;

    const std::string root = STManagerTest::create_sillytavern_fixture("manager-create-state");
    TempDirGuard root_guard(root);

    Manager manager;
    const Status create_status = Manager::create_from_root(root, &manager);
    EXPECT_TRUE(context, create_status.ok());
    EXPECT_EQ(context, manager.root_path(), root);
    EXPECT_TRUE(context, !manager.local_device_id().empty());
    EXPECT_TRUE(context, !manager.local_device_name().empty());
    EXPECT_TRUE(context, STManagerTest::path_exists(
        STManagerTest::join_path(root, ".stmanager/device_id")));
    EXPECT_TRUE(context, manager.state_dir().find(".stmanager") != std::string::npos);

    return context.failed_assertions == 0;
}

bool test_manager_create_from_root_reuses_existing_device_id() {
    TestContext context;

    const std::string root = STManagerTest::create_sillytavern_fixture("manager-reuse-device-id");
    TempDirGuard root_guard(root);

    EXPECT_TRUE(context, STManagerTest::create_directories(STManagerTest::join_path(root, ".stmanager")));
    EXPECT_TRUE(context, STManagerTest::write_file(
        STManagerTest::join_path(root, ".stmanager/device_id"),
        "fixed-device-id\n"));

    Manager manager;
    const Status create_status = Manager::create_from_root(root, &manager);
    EXPECT_TRUE(context, create_status.ok());
    EXPECT_EQ(context, manager.local_device_id(), std::string("fixed-device-id"));

    return context.failed_assertions == 0;
}

bool test_manager_resolve_pair_target_direct_host_defaults_port() {
    TestContext context;

    const std::string root = STManagerTest::create_sillytavern_fixture("manager-resolve-direct");
    TempDirGuard root_guard(root);

    Manager manager;
    const Status create_status = Manager::create_from_root(root, &manager);
    EXPECT_TRUE(context, create_status.ok());

    PairSyncRequest request;
    request.device_id = "device-direct";
    request.host = "127.0.0.1";
    request.port = 0;

    std::vector<DeviceInfo> candidates;
    DeviceInfo auto_selected;
    const Status resolve_status = manager.resolve_pair_target(request, &candidates, &auto_selected);
    EXPECT_TRUE(context, resolve_status.ok());
    EXPECT_EQ(context, candidates.size(), static_cast<size_t>(1));
    EXPECT_EQ(context, auto_selected.device_id, std::string("device-direct"));
    EXPECT_EQ(context, auto_selected.host, std::string("127.0.0.1"));
    EXPECT_EQ(context, auto_selected.port, 38591);

    return context.failed_assertions == 0;
}

bool test_manager_pair_sync_rejects_invalid_remote_endpoint() {
    TestContext context;

    const std::string root = STManagerTest::create_sillytavern_fixture("manager-pair-invalid-endpoint");
    TempDirGuard root_guard(root);

    Manager manager;
    const Status create_status = Manager::create_from_root(root, &manager);
    EXPECT_TRUE(context, create_status.ok());

    DeviceInfo remote_device;
    remote_device.device_id = "device-invalid";
    remote_device.host = "0.0.0.0";
    remote_device.port = 38591;

    PairSyncOptions options;
    std::unique_ptr<STManager::SyncTaskHandle> pair_handle = manager.pair_sync(remote_device, options);
    EXPECT_TRUE(context, pair_handle.get() != NULL);

    const Status pair_status = pair_handle->wait();
    EXPECT_TRUE(context, !pair_status.ok());
    EXPECT_EQ(context, static_cast<int>(pair_status.code), static_cast<int>(StatusCode::kSyncProtocolError));
    EXPECT_TRUE(
        context,
        static_cast<int>(pair_handle->state()) == static_cast<int>(STManager::SyncTaskState::kFinished));

    return context.failed_assertions == 0;
}

bool test_manager_export_backup_and_restore_backup_roundtrip() {
    TestContext context;

    const std::string source_root = STManagerTest::create_sillytavern_fixture("manager-export-src");
    const std::string restore_root = STManagerTest::create_sillytavern_fixture("manager-export-dst");
    TempDirGuard source_guard(source_root);
    TempDirGuard restore_guard(restore_root);

    EXPECT_TRUE(
        context,
        STManagerTest::write_file(
            STManagerTest::join_path(source_root, "data/config.json"),
            "{\"profile\":\"primary\"}"));
    EXPECT_TRUE(
        context,
        STManagerTest::write_file(
            STManagerTest::join_path(source_root, "public/scripts/extensions/ext-A/index.js"),
            "console.log('A');"));
    EXPECT_TRUE(
        context,
        STManagerTest::write_file(
            STManagerTest::join_path(restore_root, "data/stale.json"),
            "{\"stale\":true}"));

    Manager source_manager;
    Manager destination_manager;
    EXPECT_STATUS_OK(context, Manager::create_from_root(source_root, &source_manager));
    EXPECT_STATUS_OK(context, Manager::create_from_root(restore_root, &destination_manager));

    ExportBackupOptions export_options;
    export_options.file_path = STManagerTest::join_path(source_root, "st-backup.tar.zst");
    ExportBackupResult export_result;
    EXPECT_STATUS_OK(context, source_manager.export_backup(export_options, &export_result));
    EXPECT_TRUE(context, STManagerTest::path_exists(export_result.file_path));
    EXPECT_TRUE(context, export_result.bytes_written > 0);

    RestoreBackupOptions restore_options;
    restore_options.file_path = export_result.file_path;
    EXPECT_STATUS_OK(context, destination_manager.restore_backup(restore_options));

    EXPECT_EQ(
        context,
        STManagerTest::read_file(STManagerTest::join_path(restore_root, "data/config.json")),
        std::string("{\"profile\":\"primary\"}"));
    EXPECT_EQ(
        context,
        STManagerTest::read_file(
            STManagerTest::join_path(restore_root, "public/scripts/extensions/ext-A/index.js")),
        std::string("console.log('A');"));
    EXPECT_TRUE(context, !STManagerTest::path_exists(STManagerTest::join_path(restore_root, "data/stale.json")));

    return context.failed_assertions == 0;
}

bool test_manager_restore_backup_rejects_missing_file() {
    TestContext context;

    const std::string root = STManagerTest::create_sillytavern_fixture("manager-restore-missing-file");
    TempDirGuard root_guard(root);

    Manager manager;
    EXPECT_STATUS_OK(context, Manager::create_from_root(root, &manager));

    RestoreBackupOptions restore_options;
    restore_options.file_path = STManagerTest::join_path(root, "missing-backup.tar.zst");
    const Status restore_status = manager.restore_backup(restore_options);
    EXPECT_TRUE(context, !restore_status.ok());
    EXPECT_EQ(context, static_cast<int>(restore_status.code), static_cast<int>(StatusCode::kIoError));

    return context.failed_assertions == 0;
}

struct TestCase {
    const char* name;
    bool (*fn)();
};

}  // namespace

int main() {
    const TestCase test_cases[] = {
        {"locate_success_with_required_dirs", test_locate_success_with_required_dirs},
        {"locate_missing_extensions_fails", test_locate_missing_extensions_fails},
        {"locate_missing_data_fails", test_locate_missing_data_fails},
        {"backup_restore_roundtrip_data_and_extensions",
         test_backup_restore_roundtrip_data_and_extensions},
        {"backup_git_mode_skips_git_extensions_and_writes_manifest",
         test_backup_git_mode_skips_git_extensions_and_writes_manifest},
        {"backup_archive_stream_validates_successfully",
         test_backup_archive_stream_validates_successfully},
        {"backup_archive_with_nested_directories_validates_successfully",
         test_backup_archive_with_nested_directories_validates_successfully},
        {"restore_rejects_traversal_entry", test_restore_rejects_traversal_entry},
        {"restore_full_replace_removes_stale_files", test_restore_full_replace_removes_stale_files},
        {"restore_to_sillytavern_layout_replaces_public_extensions",
         test_restore_to_sillytavern_layout_replaces_public_extensions},
        {"sync_push_requires_trusted_device", test_sync_push_requires_trusted_device},
        {"sync_pair_then_push_success", test_sync_pair_then_push_success},
        {"sync_pull_restores_to_override_root", test_sync_pull_restores_to_override_root},
        {"discover_devices_autostarts_discovery", test_discover_devices_autostarts_discovery},
        {"manager_create_from_root_creates_state", test_manager_create_from_root_creates_state},
        {"manager_create_from_root_reuses_existing_device_id", test_manager_create_from_root_reuses_existing_device_id},
        {"manager_resolve_pair_target_direct_host_defaults_port", test_manager_resolve_pair_target_direct_host_defaults_port},
        {"manager_pair_sync_rejects_invalid_remote_endpoint", test_manager_pair_sync_rejects_invalid_remote_endpoint},
        {"manager_export_backup_and_restore_backup_roundtrip",
         test_manager_export_backup_and_restore_backup_roundtrip},
        {"manager_restore_backup_rejects_missing_file", test_manager_restore_backup_rejects_missing_file},
    };

    int passed_count = 0;
    int failed_count = 0;

    for (size_t index = 0; index < sizeof(test_cases) / sizeof(test_cases[0]); ++index) {
        const TestCase& test_case = test_cases[index];
        const bool passed = test_case.fn();

        if (passed) {
            ++passed_count;
            std::cout << "[PASS] " << test_case.name << "\n";
        } else {
            ++failed_count;
            std::cout << "[FAIL] " << test_case.name << "\n";
        }
    }

    std::cout << "\nSummary: " << passed_count << " passed, " << failed_count << " failed\n";
    return failed_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
