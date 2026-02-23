#ifndef STMANAGER_PATH_SAFETY_HPP
#define STMANAGER_PATH_SAFETY_HPP

#include <STManager/data.h>

#include <string>

namespace STManager {
namespace internal {

Status validate_archive_relative_path(const std::string& archive_path);
Status join_destination_path(
    const std::string& destination_root,
    const std::string& archive_path,
    std::string* destination_path);
Status ensure_directory_tree(const std::string& directory_path, int mode);
Status ensure_parent_directories(const std::string& file_path, int mode);

}  // namespace internal
}  // namespace STManager

#endif
