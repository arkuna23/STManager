#include <STManager/data.h>
#include <STManager/sync.h>
#include <STManager/tcp_transport.h>

#include "test_helpers.h"

#include <iostream>
#include <sstream>
#include <string>

using STManager::DataManager;
using STManager::JsonTrustedDeviceStore;
using STManager::ServerOptions;
using STManager::Status;
using STManager::StatusCode;
using STManager::TcpSyncTransport;

namespace {

struct TestContext {
    int failed_assertions;
    TestContext() : failed_assertions(0) {}
};

#define EXPECT_TRUE(ctx, expr)                                                                    \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__ << ": " #expr \
                      << "\n";                                                                   \
            ++(ctx).failed_assertions;                                                             \
        }                                                                                          \
    } while (0)

#define EXPECT_EQ(ctx, lhs, rhs)                                                                    \
    do {                                                                                             \
        const auto _lhs_value = (lhs);                                                              \
        const auto _rhs_value = (rhs);                                                              \
        if (!(_lhs_value == _rhs_value)) {                                                          \
            std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__ << ": " #lhs   \
                      << " == " #rhs << " (actual: " << _lhs_value << ", expected: "          \
                      << _rhs_value << ")\n";                                                     \
            ++(ctx).failed_assertions;                                                               \
        }                                                                                            \
    } while (0)

bool test_tcp_transport_rejects_invalid_connect_args() {
    TestContext context;

    TcpSyncTransport transport;
    STManager::DeviceInfo invalid_host;
    invalid_host.device_id = "device-a";
    invalid_host.port = 12345;

    const Status invalid_host_status = transport.connect(invalid_host);
    EXPECT_TRUE(context, !invalid_host_status.ok());
    EXPECT_EQ(context, static_cast<int>(invalid_host_status.code), static_cast<int>(StatusCode::kSyncProtocolError));

    STManager::DeviceInfo invalid_port;
    invalid_port.device_id = "device-a";
    invalid_port.host = "127.0.0.1";
    invalid_port.port = 0;

    const Status invalid_port_status = transport.connect(invalid_port);
    EXPECT_TRUE(context, !invalid_port_status.ok());
    EXPECT_EQ(context, static_cast<int>(invalid_port_status.code), static_cast<int>(StatusCode::kSyncProtocolError));
    return context.failed_assertions == 0;
}

bool test_tcp_transport_fails_when_disconnected() {
    TestContext context;

    TcpSyncTransport transport;

    std::string message;
    const Status send_message_status = transport.send_message("hello");
    const Status receive_message_status = transport.receive_message(&message);
    EXPECT_TRUE(context, !send_message_status.ok());
    EXPECT_TRUE(context, !receive_message_status.ok());
    EXPECT_EQ(context, static_cast<int>(send_message_status.code), static_cast<int>(StatusCode::kSyncProtocolError));
    EXPECT_EQ(
        context,
        static_cast<int>(receive_message_status.code),
        static_cast<int>(StatusCode::kSyncProtocolError));

    std::stringstream in_stream(std::ios::in | std::ios::out | std::ios::binary);
    in_stream.write("abc", 3);
    in_stream.seekg(0, std::ios::beg);
    std::stringstream out_stream(std::ios::in | std::ios::out | std::ios::binary);

    const Status send_stream_status = transport.send_stream(in_stream);
    const Status receive_stream_status = transport.receive_stream(out_stream);
    EXPECT_TRUE(context, !send_stream_status.ok());
    EXPECT_TRUE(context, !receive_stream_status.ok());
    EXPECT_EQ(context, static_cast<int>(send_stream_status.code), static_cast<int>(StatusCode::kSyncProtocolError));
    EXPECT_EQ(
        context,
        static_cast<int>(receive_stream_status.code),
        static_cast<int>(StatusCode::kSyncProtocolError));
    return context.failed_assertions == 0;
}

bool test_run_sync_server_rejects_invalid_data_manager() {
    TestContext context;

    const DataManager invalid_manager("not-a-valid-root");
    ServerOptions options;
    options.advertise = false;
    options.port = 0;
    int bound_port = 0;

    JsonTrustedDeviceStore trusted_store("/tmp/stmanager-trusted-unused.json");
    const Status run_status = STManager::run_sync_server(
        invalid_manager,
        "server-id",
        &trusted_store,
        options,
        &bound_port);

    EXPECT_TRUE(context, !run_status.ok());
    EXPECT_EQ(context, static_cast<int>(run_status.code), static_cast<int>(StatusCode::kInvalidRoot));
    return context.failed_assertions == 0;
}

bool test_run_sync_server_rejects_empty_local_device_id() {
    TestContext context;

    const std::string root_path = STManagerTest::create_sillytavern_fixture("server-empty-device-id");
    EXPECT_TRUE(context, !root_path.empty());

    const DataManager manager = DataManager::locate(root_path);
    EXPECT_TRUE(context, manager.is_valid());

    ServerOptions options;
    options.advertise = false;
    options.port = 0;
    int bound_port = 0;
    JsonTrustedDeviceStore trusted_store("/tmp/stmanager-trusted-unused.json");
    const Status run_status = STManager::run_sync_server(
        manager,
        "",
        &trusted_store,
        options,
        &bound_port);

    EXPECT_TRUE(context, !run_status.ok());
    EXPECT_EQ(context, static_cast<int>(run_status.code), static_cast<int>(StatusCode::kSyncProtocolError));
    STManagerTest::remove_directory_recursive(root_path);
    return context.failed_assertions == 0;
}

bool test_run_sync_server_rejects_null_bound_port() {
    TestContext context;

    const std::string root_path = STManagerTest::create_sillytavern_fixture("server-null-bound-port");
    EXPECT_TRUE(context, !root_path.empty());

    const DataManager manager = DataManager::locate(root_path);
    EXPECT_TRUE(context, manager.is_valid());

    ServerOptions options;
    options.advertise = false;
    options.port = 0;
    JsonTrustedDeviceStore trusted_store("/tmp/stmanager-trusted-unused.json");
    const Status run_status = STManager::run_sync_server(
        manager,
        "device-id",
        &trusted_store,
        options,
        NULL);

    EXPECT_TRUE(context, !run_status.ok());
    EXPECT_EQ(context, static_cast<int>(run_status.code), static_cast<int>(StatusCode::kSyncProtocolError));
    STManagerTest::remove_directory_recursive(root_path);
    return context.failed_assertions == 0;
}

struct TestCase {
    const char* name;
    bool (*fn)();
};

}  // namespace

int main() {
    const TestCase test_cases[] = {
        {"tcp_transport_rejects_invalid_connect_args", test_tcp_transport_rejects_invalid_connect_args},
        {"tcp_transport_fails_when_disconnected", test_tcp_transport_fails_when_disconnected},
        {"run_sync_server_rejects_invalid_data_manager", test_run_sync_server_rejects_invalid_data_manager},
        {"run_sync_server_rejects_empty_local_device_id", test_run_sync_server_rejects_empty_local_device_id},
        {"run_sync_server_rejects_null_bound_port", test_run_sync_server_rejects_null_bound_port},
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
