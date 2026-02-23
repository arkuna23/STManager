#ifndef STMANAGER_TEST_HELPERS_HPP
#define STMANAGER_TEST_HELPERS_HPP

#include <string>

namespace STManagerTest {

std::string join_path(const std::string& lhs, const std::string& rhs);

bool path_exists(const std::string& path);
bool is_directory(const std::string& path);

bool create_directories(const std::string& path);
bool write_file(const std::string& path, const std::string& content);
std::string read_file(const std::string& path);

bool copy_directory_recursive(const std::string& src_path, const std::string& dst_path);
bool remove_directory_recursive(const std::string& path);

std::string create_temp_directory(const std::string& prefix);
std::string create_sillytavern_fixture(const std::string& case_name);

}  // namespace STManagerTest

#endif
