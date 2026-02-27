#include <STManager/manager.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "cli_args.h"
#include "cli_command_selector.h"
#include "cli_net.h"
#include "cli_pair.h"
#include "cli_state.h"
#include "test_helpers.h"

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

bool test_parse_serve_backup_defaults() {
    TestContext context;

    char command_0[] = "stmanager";
    char command_1[] = "serve";
    char command_2[] = "backup";
    char* argv[] = {command_0, command_1, command_2};

    STManagerCli::ParsedArgs parsed_args;
    std::string error_message;
    EXPECT_TRUE(context, STManagerCli::parse_cli_args(3, argv, &parsed_args, &error_message));
    EXPECT_EQ(context, static_cast<int>(parsed_args.command_type),
              static_cast<int>(STManagerCli::CommandType::kServeBackup));
    EXPECT_EQ(context, parsed_args.serve_backup_args.bind_host, std::string("0.0.0.0"));
    EXPECT_EQ(context, parsed_args.serve_backup_args.port, STManagerCli::kDefaultSyncPort);
    EXPECT_TRUE(context, parsed_args.serve_backup_args.advertise);

    return context.failed_assertions == 0;
}

bool test_parse_pair_restore_allows_missing_device_id() {
    TestContext context;

    char command_0[] = "stmanager";
    char command_1[] = "pair";
    char command_2[] = "restore";
    char* argv[] = {command_0, command_1, command_2};

    STManagerCli::ParsedArgs parsed_args;
    std::string error_message;
    EXPECT_TRUE(context, STManagerCli::parse_cli_args(3, argv, &parsed_args, &error_message));
    EXPECT_EQ(context, static_cast<int>(parsed_args.command_type),
              static_cast<int>(STManagerCli::CommandType::kPairRestore));
    EXPECT_TRUE(context, parsed_args.pair_restore_args.device_id.empty());

    return context.failed_assertions == 0;
}

bool test_parse_serve_backup_device_name() {
    TestContext context;

    char command_0[] = "stmanager";
    char command_1[] = "serve";
    char command_2[] = "backup";
    char command_3[] = "--device-name";
    char command_4[] = "MyDevice";
    char* argv[] = {command_0, command_1, command_2, command_3, command_4};

    STManagerCli::ParsedArgs parsed_args;
    std::string error_message;
    EXPECT_TRUE(context, STManagerCli::parse_cli_args(5, argv, &parsed_args, &error_message));
    EXPECT_EQ(context, static_cast<int>(parsed_args.command_type),
              static_cast<int>(STManagerCli::CommandType::kServeBackup));
    EXPECT_EQ(context, parsed_args.serve_backup_args.device_name, std::string("MyDevice"));

    return context.failed_assertions == 0;
}

bool test_parse_pair_restore_device_name() {
    TestContext context;

    char command_0[] = "stmanager";
    char command_1[] = "pair";
    char command_2[] = "restore";
    char command_3[] = "--device-name";
    char command_4[] = "MyDevice";
    char* argv[] = {command_0, command_1, command_2, command_3, command_4};

    STManagerCli::ParsedArgs parsed_args;
    std::string error_message;
    EXPECT_TRUE(context, STManagerCli::parse_cli_args(5, argv, &parsed_args, &error_message));
    EXPECT_EQ(context, static_cast<int>(parsed_args.command_type),
              static_cast<int>(STManagerCli::CommandType::kPairRestore));
    EXPECT_EQ(context, parsed_args.pair_restore_args.device_name, std::string("MyDevice"));

    return context.failed_assertions == 0;
}

bool test_parse_export_backup_defaults() {
    TestContext context;

    char command_0[] = "stmanager";
    char command_1[] = "export";
    char command_2[] = "backup";
    char* argv[] = {command_0, command_1, command_2};

    STManagerCli::ParsedArgs parsed_args;
    std::string error_message;
    EXPECT_TRUE(context, STManagerCli::parse_cli_args(3, argv, &parsed_args, &error_message));
    EXPECT_EQ(context, static_cast<int>(parsed_args.command_type),
              static_cast<int>(STManagerCli::CommandType::kExportBackup));
    EXPECT_EQ(context, parsed_args.export_backup_args.file_path, std::string("st-backup.tar.zst"));
    EXPECT_TRUE(context, !parsed_args.export_backup_args.git_mode);

    return context.failed_assertions == 0;
}

bool test_parse_restore_backup_defaults() {
    TestContext context;

    char command_0[] = "stmanager";
    char command_1[] = "restore";
    char command_2[] = "backup";
    char* argv[] = {command_0, command_1, command_2};

    STManagerCli::ParsedArgs parsed_args;
    std::string error_message;
    EXPECT_TRUE(context, STManagerCli::parse_cli_args(3, argv, &parsed_args, &error_message));
    EXPECT_EQ(context, static_cast<int>(parsed_args.command_type),
              static_cast<int>(STManagerCli::CommandType::kRestoreBackup));
    EXPECT_EQ(context, parsed_args.restore_backup_args.file_path, std::string("st-backup.tar.zst"));

    return context.failed_assertions == 0;
}

bool test_parse_help_options() {
    TestContext context;

    char command_0[] = "stmanager";
    char command_1[] = "--help";
    char* argv_long[] = {command_0, command_1};

    STManagerCli::ParsedArgs parsed_args;
    std::string error_message;
    EXPECT_TRUE(context, STManagerCli::parse_cli_args(2, argv_long, &parsed_args, &error_message));
    EXPECT_EQ(context, static_cast<int>(parsed_args.command_type),
              static_cast<int>(STManagerCli::CommandType::kHelp));

    char command_2[] = "-h";
    char* argv_short[] = {command_0, command_2};
    parsed_args = STManagerCli::ParsedArgs();
    error_message.clear();
    EXPECT_TRUE(context, STManagerCli::parse_cli_args(2, argv_short, &parsed_args, &error_message));
    EXPECT_EQ(context, static_cast<int>(parsed_args.command_type),
              static_cast<int>(STManagerCli::CommandType::kHelp));

    char command_3[] = "serve";
    char command_4[] = "backup";
    char command_5[] = "--help";
    char* argv_subcommand[] = {command_0, command_3, command_4, command_5};
    parsed_args = STManagerCli::ParsedArgs();
    error_message.clear();
    EXPECT_TRUE(context,
                STManagerCli::parse_cli_args(4, argv_subcommand, &parsed_args, &error_message));
    EXPECT_EQ(context, static_cast<int>(parsed_args.command_type),
              static_cast<int>(STManagerCli::CommandType::kHelp));

    return context.failed_assertions == 0;
}

bool test_help_text_mentions_device_name_options() {
    TestContext context;

    const std::string help_text = STManagerCli::build_help_text();
    EXPECT_TRUE(context, help_text.find("serve backup") != std::string::npos);
    EXPECT_TRUE(context, help_text.find("pair restore") != std::string::npos);
    EXPECT_TRUE(context, help_text.find("--device-name <name>") != std::string::npos);

    return context.failed_assertions == 0;
}

bool test_parse_serve_without_action_fails() {
    TestContext context;

    char command_0[] = "stmanager";
    char command_1[] = "serve";
    char* argv[] = {command_0, command_1};

    STManagerCli::ParsedArgs parsed_args;
    std::string error_message;
    EXPECT_TRUE(context, !STManagerCli::parse_cli_args(2, argv, &parsed_args, &error_message));
    EXPECT_EQ(context, error_message, std::string("Missing action"));

    return context.failed_assertions == 0;
}

bool test_detect_sillytavern_root_from_parent() {
    TestContext context;

    const std::string fixture_root = STManagerTest::create_sillytavern_fixture("cli-root-detect");
    TempDirGuard fixture_guard(fixture_root);
    WorkingDirGuard cwd_guard;

    const std::string nested_path =
        STManagerTest::join_path(fixture_root, "public/scripts/extensions/third-party");
    EXPECT_TRUE(context, chdir(nested_path.c_str()) == 0);

    std::string resolved_root;
    std::string error_message;
    EXPECT_TRUE(context, STManagerCli::detect_sillytavern_root("", &resolved_root, &error_message));
    EXPECT_TRUE(context, !resolved_root.empty());
    EXPECT_TRUE(context, STManagerTest::is_directory(resolved_root));
    EXPECT_TRUE(context,
                STManagerTest::is_directory(STManagerTest::join_path(resolved_root, "data")));
    EXPECT_TRUE(context, STManagerTest::is_directory(STManagerTest::join_path(
                             resolved_root, "public/scripts/extensions/third-party")));

    return context.failed_assertions == 0;
}

bool test_manager_create_from_root_creates_device_id() {
    TestContext context;

    const std::string fixture_root = STManagerTest::create_sillytavern_fixture("cli-manager-state");
    TempDirGuard fixture_guard(fixture_root);

    STManager::Manager manager;
    const STManager::Status create_status =
        STManager::Manager::create_from_root(fixture_root, &manager);
    EXPECT_TRUE(context, create_status.ok());
    EXPECT_TRUE(context, !manager.local_device_id().empty());
    EXPECT_TRUE(context, !manager.local_device_name().empty());
    EXPECT_TRUE(context, manager.state_dir().find(".stmanager") != std::string::npos);
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

bool test_select_pair_device_prompts_for_single_candidate() {
    TestContext context;

    STManagerCli::PairRestoreArgs pair_args;

    STManager::DeviceInfo candidate;
    candidate.device_id = "device-a";
    candidate.device_name = "device-a";
    candidate.host = "192.168.1.10";
    candidate.port = 38591;

    std::vector<STManager::DeviceInfo> candidates(1, candidate);
    std::istringstream input_stream("1\n");
    std::ostringstream output_stream;
    std::string error_message;
    STManager::DeviceInfo selected_device;

    const bool selected =
        STManagerCli::select_pair_device(pair_args, candidates, candidate, input_stream,
                                         output_stream, &error_message, &selected_device);

    EXPECT_TRUE(context, selected);
    EXPECT_TRUE(context, error_message.empty());
    EXPECT_EQ(context, selected_device.device_id, std::string("device-a"));
    EXPECT_TRUE(context, output_stream.str().find("Discovered devices:\n") != std::string::npos);
    EXPECT_TRUE(context, output_stream.str().find("Select device [1-1]: ") != std::string::npos);

    return context.failed_assertions == 0;
}

bool test_select_pair_device_rejects_invalid_selection() {
    TestContext context;

    STManagerCli::PairRestoreArgs pair_args;

    STManager::DeviceInfo first_candidate;
    first_candidate.device_id = "device-a";
    first_candidate.device_name = "device-a";
    first_candidate.host = "192.168.1.10";
    first_candidate.port = 38591;

    STManager::DeviceInfo second_candidate = first_candidate;
    second_candidate.device_id = "device-b";
    second_candidate.device_name = "device-b";
    second_candidate.host = "192.168.1.11";

    std::vector<STManager::DeviceInfo> candidates;
    candidates.push_back(first_candidate);
    candidates.push_back(second_candidate);

    std::istringstream input_stream("9\n");
    std::ostringstream output_stream;
    std::string error_message;
    STManager::DeviceInfo selected_device;

    const bool selected =
        STManagerCli::select_pair_device(pair_args, candidates, first_candidate, input_stream,
                                         output_stream, &error_message, &selected_device);

    EXPECT_TRUE(context, !selected);
    EXPECT_TRUE(
        context,
        error_message == "Invalid selection. Please rerun pair and choose a valid device index.");

    return context.failed_assertions == 0;
}

bool test_select_pair_device_skips_prompt_with_explicit_device_id() {
    TestContext context;

    STManagerCli::PairRestoreArgs pair_args;
    pair_args.device_id = "device-a";

    STManager::DeviceInfo candidate;
    candidate.device_id = "device-a";
    candidate.device_name = "device-a";
    candidate.host = "192.168.1.10";
    candidate.port = 38591;

    std::vector<STManager::DeviceInfo> candidates(1, candidate);
    std::istringstream input_stream("");
    std::ostringstream output_stream;
    std::string error_message;
    STManager::DeviceInfo selected_device;

    const bool selected =
        STManagerCli::select_pair_device(pair_args, candidates, candidate, input_stream,
                                         output_stream, &error_message, &selected_device);

    EXPECT_TRUE(context, selected);
    EXPECT_TRUE(context, error_message.empty());
    EXPECT_EQ(context, selected_device.device_id, std::string("device-a"));
    EXPECT_TRUE(context, output_stream.str().empty());

    return context.failed_assertions == 0;
}

bool test_select_pair_device_rejects_unconnectable_endpoint() {
    TestContext context;

    STManagerCli::PairRestoreArgs pair_args;

    STManager::DeviceInfo candidate;
    candidate.device_id = "device-a";
    candidate.device_name = "device-a";
    candidate.host = "0.0.0.0";
    candidate.port = 38591;

    std::vector<STManager::DeviceInfo> candidates(1, candidate);
    std::istringstream input_stream("1\n");
    std::ostringstream output_stream;
    std::string error_message;
    STManager::DeviceInfo selected_device;

    const bool selected =
        STManagerCli::select_pair_device(pair_args, candidates, candidate, input_stream,
                                         output_stream, &error_message, &selected_device);

    EXPECT_TRUE(context, !selected);
    EXPECT_TRUE(context, error_message ==
                             "Discovered remote endpoint is not connectable. "
                             "Use --host <device_ip> and optional --port to connect directly.");

    return context.failed_assertions == 0;
}

bool test_select_action_serve_backup() {
    TestContext context;

    std::istringstream input_stream("1\n");
    std::ostringstream output_stream;
    std::string error_message;
    STManagerCli::CommandType command_type = STManagerCli::CommandType::kUnknown;

    const bool selected =
        STManagerCli::select_action(input_stream, output_stream, &error_message, &command_type);

    EXPECT_TRUE(context, selected);
    EXPECT_TRUE(context, error_message.empty());
    EXPECT_EQ(context, static_cast<int>(command_type),
              static_cast<int>(STManagerCli::CommandType::kServeBackup));
    EXPECT_TRUE(context, output_stream.str().find("Select action:\n") != std::string::npos);
    EXPECT_TRUE(context, output_stream.str().find("  [1] serve backup\n") != std::string::npos);
    EXPECT_TRUE(context, output_stream.str().find("Select action [1-4]: ") != std::string::npos);

    return context.failed_assertions == 0;
}

bool test_select_action_restore_backup() {
    TestContext context;

    std::istringstream input_stream("4\n");
    std::ostringstream output_stream;
    std::string error_message;
    STManagerCli::CommandType command_type = STManagerCli::CommandType::kUnknown;

    const bool selected =
        STManagerCli::select_action(input_stream, output_stream, &error_message, &command_type);

    EXPECT_TRUE(context, selected);
    EXPECT_TRUE(context, error_message.empty());
    EXPECT_EQ(context, static_cast<int>(command_type),
              static_cast<int>(STManagerCli::CommandType::kRestoreBackup));

    return context.failed_assertions == 0;
}

bool test_select_action_rejects_invalid_input() {
    TestContext context;

    std::istringstream input_stream("x\n");
    std::ostringstream output_stream;
    std::string error_message;
    STManagerCli::CommandType command_type = STManagerCli::CommandType::kUnknown;

    const bool selected =
        STManagerCli::select_action(input_stream, output_stream, &error_message, &command_type);

    EXPECT_TRUE(context, !selected);
    EXPECT_TRUE(context, error_message == "Invalid action selection. Please enter 1 to 4.");
    EXPECT_EQ(context, static_cast<int>(command_type),
              static_cast<int>(STManagerCli::CommandType::kUnknown));

    return context.failed_assertions == 0;
}

struct TestCase {
    const char* name;
    bool (*fn)();
};

}  // namespace

int main() {
    const TestCase test_cases[] = {
        {"parse_serve_backup_defaults", test_parse_serve_backup_defaults},
        {"parse_pair_restore_allows_missing_device_id",
         test_parse_pair_restore_allows_missing_device_id},
        {"parse_serve_backup_device_name", test_parse_serve_backup_device_name},
        {"parse_pair_restore_device_name", test_parse_pair_restore_device_name},
        {"parse_export_backup_defaults", test_parse_export_backup_defaults},
        {"parse_restore_backup_defaults", test_parse_restore_backup_defaults},
        {"parse_help_options", test_parse_help_options},
        {"help_text_mentions_device_name_options", test_help_text_mentions_device_name_options},
        {"parse_serve_without_action_fails", test_parse_serve_without_action_fails},
#ifndef _WIN32
        {"detect_sillytavern_root_from_parent", test_detect_sillytavern_root_from_parent},
#endif
        {"manager_create_from_root_creates_device_id",
         test_manager_create_from_root_creates_device_id},
        {"is_connectable_host_rejects_wildcard", test_is_connectable_host_rejects_wildcard},
        {"select_pair_device_prompts_for_single_candidate",
         test_select_pair_device_prompts_for_single_candidate},
        {"select_pair_device_rejects_invalid_selection",
         test_select_pair_device_rejects_invalid_selection},
        {"select_pair_device_skips_prompt_with_explicit_device_id",
         test_select_pair_device_skips_prompt_with_explicit_device_id},
        {"select_pair_device_rejects_unconnectable_endpoint",
         test_select_pair_device_rejects_unconnectable_endpoint},
        {"select_action_serve_backup", test_select_action_serve_backup},
        {"select_action_restore_backup", test_select_action_restore_backup},
        {"select_action_rejects_invalid_input", test_select_action_rejects_invalid_input},
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
