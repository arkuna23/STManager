#include "test_helpers.h"

#include "cli_args.h"
#include "cli_net.h"
#include "cli_state.h"

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

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

struct TempDirGuard {
    std::string path;
    explicit TempDirGuard(const std::string& path_in) : path(path_in) {}
    ~TempDirGuard() {
        if (!path.empty()) {
            STManagerTest::remove_directory_recursive(path);
        }
    }
};

class WorkingDirGuard {
public:
    WorkingDirGuard() : original_path_() {
        char buffer[4096];
        if (getcwd(buffer, sizeof(buffer)) != NULL) {
            original_path_ = buffer;
        }
    }

    ~WorkingDirGuard() {
        if (!original_path_.empty()) {
            chdir(original_path_.c_str());
        }
    }

private:
    std::string original_path_;
};

bool test_parse_run_defaults() {
    TestContext context;

    char command_0[] = "stmanager";
    char command_1[] = "run";
    char* argv[] = {command_0, command_1};

    STManagerCli::ParsedArgs parsed_args;
    std::string error_message;
    EXPECT_TRUE(context, STManagerCli::parse_cli_args(2, argv, &parsed_args, &error_message));
    EXPECT_EQ(context, static_cast<int>(parsed_args.command_type), static_cast<int>(STManagerCli::CommandType::kRun));
    EXPECT_EQ(context, parsed_args.run_args.bind_host, std::string("0.0.0.0"));
    EXPECT_EQ(context, parsed_args.run_args.port, STManagerCli::kDefaultSyncPort);
    EXPECT_TRUE(context, parsed_args.run_args.advertise);

    return context.failed_assertions == 0;
}

bool test_parse_pair_allows_missing_device_id() {
    TestContext context;

    char command_0[] = "stmanager";
    char command_1[] = "pair";
    char* argv[] = {command_0, command_1};

    STManagerCli::ParsedArgs parsed_args;
    std::string error_message;
    EXPECT_TRUE(context, STManagerCli::parse_cli_args(2, argv, &parsed_args, &error_message));
    EXPECT_EQ(context, static_cast<int>(parsed_args.command_type), static_cast<int>(STManagerCli::CommandType::kPair));
    EXPECT_TRUE(context, parsed_args.pair_args.device_id.empty());

    return context.failed_assertions == 0;
}

bool test_detect_sillytavern_root_from_parent() {
    TestContext context;

    const std::string fixture_root = STManagerTest::create_sillytavern_fixture("cli-root-detect");
    TempDirGuard fixture_guard(fixture_root);
    WorkingDirGuard cwd_guard;

    const std::string nested_path = STManagerTest::join_path(
        fixture_root,
        "public/scripts/extensions");
    EXPECT_TRUE(context, chdir(nested_path.c_str()) == 0);

    std::string resolved_root;
    std::string error_message;
    EXPECT_TRUE(context, STManagerCli::detect_sillytavern_root("", &resolved_root, &error_message));
    EXPECT_EQ(context, resolved_root, fixture_root);

    return context.failed_assertions == 0;
}

bool test_init_local_state_creates_device_id() {
    TestContext context;

    const std::string fixture_root = STManagerTest::create_sillytavern_fixture("cli-local-state");
    TempDirGuard fixture_guard(fixture_root);

    std::string local_device_id;
    std::string trusted_store_path;
    std::string error_message;
    EXPECT_TRUE(context, STManagerCli::init_local_state(
        fixture_root,
        &local_device_id,
        &trusted_store_path,
        &error_message));
    EXPECT_TRUE(context, !local_device_id.empty());
    EXPECT_TRUE(context, trusted_store_path.find(".stmanager/trusted_devices.json") != std::string::npos);
    EXPECT_TRUE(context, STManagerTest::path_exists(
        STManagerTest::join_path(fixture_root, ".stmanager/device_id")));

    return context.failed_assertions == 0;
}

bool test_is_connectable_host_rejects_wildcard() {
    TestContext context;

    EXPECT_TRUE(context, !STManagerCli::is_connectable_host(""));
    EXPECT_TRUE(context, !STManagerCli::is_connectable_host("0.0.0.0"));
    EXPECT_TRUE(context, STManagerCli::is_connectable_host("127.0.0.1"));
    EXPECT_TRUE(context, STManagerCli::is_connectable_host("192.168.1.20"));

    return context.failed_assertions == 0;
}

struct TestCase {
    const char* name;
    bool (*fn)();
};

}  // namespace

int main() {
    const TestCase test_cases[] = {
        {"parse_run_defaults", test_parse_run_defaults},
        {"parse_pair_allows_missing_device_id", test_parse_pair_allows_missing_device_id},
        {"detect_sillytavern_root_from_parent", test_detect_sillytavern_root_from_parent},
        {"init_local_state_creates_device_id", test_init_local_state_creates_device_id},
        {"is_connectable_host_rejects_wildcard", test_is_connectable_host_rejects_wildcard},
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
