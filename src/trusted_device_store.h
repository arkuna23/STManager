#ifndef STMANAGER_TRUSTED_DEVICE_STORE_HPP
#define STMANAGER_TRUSTED_DEVICE_STORE_HPP

#include <STManager/data.h>

#include <string>
#include <vector>

namespace STManager {
namespace internal {

bool contains_device_id(const std::vector<std::string>& device_ids, const std::string& device_id);
void add_device_id(std::vector<std::string>* device_ids, const std::string& device_id);
void remove_device_id(std::vector<std::string>* device_ids, const std::string& device_id);

Status load_trusted_device_ids(const std::string& store_path, std::vector<std::string>* trusted_device_ids);
Status save_trusted_device_ids(const std::string& store_path, const std::vector<std::string>& trusted_device_ids);

}  // namespace internal
}  // namespace STManager

#endif
