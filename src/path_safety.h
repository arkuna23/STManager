#ifndef STMANAGER_PATH_SAFETY_HPP
#define STMANAGER_PATH_SAFETY_HPP

#include <STManager/data.h>

#include <string>

namespace STManager {
namespace internal {

Status validate_archive_relative_path(const std::string& archive_path);
Status join_destination_path(const std::string& destination_root, const std::string& archive_path,
                             std::string* destination_path);
Status ensure_directory_tree(const std::string& directory_path, int mode);
Status ensure_parent_directories(const std::string& file_path, int mode);
Status ensure_directory_tree_following_existing_symlinks(const std::string& directory_path,
                                                         int mode);
Status ensure_parent_directories_following_existing_symlinks(const std::string& file_path,
                                                             int mode);
Status ensure_archive_entry_directory(const std::string& destination_root,
                                      const std::string& archive_path, int mode);
Status ensure_archive_entry_parent_directories(const std::string& destination_root,
                                               const std::string& archive_path, int mode);
Status reject_existing_symlink(const std::string& path);

}  // namespace internal
}  // namespace STManager

#endif
