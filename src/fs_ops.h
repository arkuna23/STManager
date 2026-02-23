#ifndef STMANAGER_FS_OPS_HPP
#define STMANAGER_FS_OPS_HPP

#include <STManager/data.h>

#include <string>

namespace STManager {
namespace internal {

bool path_exists(const std::string& path);
bool path_is_directory(const std::string& path);

Status create_temp_directory_under(const std::string& base_directory, const std::string& prefix,
                                   std::string* temp_directory);

Status remove_path_recursive(const std::string& path);
Status copy_path_recursive(const std::string& source_path, const std::string& destination_path);
Status move_or_copy_path(const std::string& source_path, const std::string& destination_path);

}  // namespace internal
}  // namespace STManager

#endif
