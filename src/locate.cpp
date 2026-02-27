#include "locate.h"
#include "path_safety.h"

#include <sys/stat.h>

namespace STManager {
namespace internal {
namespace {

bool is_directory(const std::string& path) {
    struct stat path_stat;
    if (stat(path.c_str(), &path_stat) != 0) {
        return false;
    }
    return S_ISDIR(path_stat.st_mode);
}

std::string join_path(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (lhs[lhs.size() - 1] == '/') {
        return lhs + rhs;
    }
    return lhs + "/" + rhs;
}

}  // namespace

Status locate_silly_tavern_paths(const std::string& root_path, std::string* resolved_root_path,
                                 std::string* extensions_path, std::string* data_path) {
    if (!is_directory(root_path)) {
        return Status(StatusCode::kInvalidRoot,
                      "SillyTavern root does not exist or is not a directory");
    }

    const std::string extensions_root_path = join_path(root_path, "public/scripts/extensions");
    if (!is_directory(extensions_root_path)) {
        return Status(StatusCode::kMissingExtensionsDir,
                      "Missing required directory: public/scripts/extensions");
    }

    const std::string candidate_extensions_path = join_path(extensions_root_path, "third-party");
    if (!is_directory(candidate_extensions_path)) {
        const Status create_status = ensure_directory_tree(candidate_extensions_path, 0755);
        if (!create_status.ok()) {
            return Status(
                StatusCode::kMissingExtensionsDir,
                "Missing required directory: public/scripts/extensions/third-party; auto-create failed: " +
                    create_status.message);
        }
    }

    const std::string candidate_data_path = join_path(root_path, "data");
    if (!is_directory(candidate_data_path)) {
        return Status(StatusCode::kMissingDataDir, "Missing required directory: data");
    }

    *resolved_root_path = root_path;
    *extensions_path = candidate_extensions_path;
    *data_path = candidate_data_path;
    return Status::ok_status();
}

}  // namespace internal
}  // namespace STManager
