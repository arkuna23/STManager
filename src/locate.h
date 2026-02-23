#ifndef STMANAGER_LOCATE_HPP
#define STMANAGER_LOCATE_HPP

#include <STManager/data.h>

#include <string>

namespace STManager {
namespace internal {

Status locate_silly_tavern_paths(
    const std::string& root_path,
    std::string* resolved_root_path,
    std::string* extensions_path,
    std::string* data_path);

}  // namespace internal
}  // namespace STManager

#endif
