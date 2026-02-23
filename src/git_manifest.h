#ifndef STMANAGER_GIT_MANIFEST_HPP
#define STMANAGER_GIT_MANIFEST_HPP

#include <STManager/data.h>

#include <set>
#include <string>
#include <vector>

namespace STManager {
namespace internal {

Status collect_git_extension_info(
    const std::string& extensions_path,
    std::vector<GitExtensionInfo>* git_extensions,
    std::set<std::string>* git_extension_names);

std::string build_git_manifest_json(const std::vector<GitExtensionInfo>& git_extensions);

}  // namespace internal
}  // namespace STManager

#endif
