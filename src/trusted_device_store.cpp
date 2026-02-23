#include "trusted_device_store.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace STManager {
namespace internal {

bool contains_device_id(const std::vector<std::string>& device_ids, const std::string& device_id) {
    return std::find(device_ids.begin(), device_ids.end(), device_id) != device_ids.end();
}

void add_device_id(std::vector<std::string>* device_ids, const std::string& device_id) {
    if (contains_device_id(*device_ids, device_id)) {
        return;
    }
    device_ids->push_back(device_id);
}

void remove_device_id(std::vector<std::string>* device_ids, const std::string& device_id) {
    std::vector<std::string>::iterator it = std::remove(device_ids->begin(), device_ids->end(), device_id);
    device_ids->erase(it, device_ids->end());
}

Status load_trusted_device_ids(const std::string& store_path, std::vector<std::string>* trusted_device_ids) {
    trusted_device_ids->clear();

    std::ifstream in_file(store_path.c_str());
    if (!in_file.is_open()) {
        return Status::ok_status();
    }

    try {
        nlohmann::json store_json;
        in_file >> store_json;

        if (!store_json.is_object()) {
            return Status(StatusCode::kIoError, "Invalid trusted device store json object");
        }

        const nlohmann::json device_ids_json = store_json.value("trusted_device_ids", nlohmann::json::array());
        if (!device_ids_json.is_array()) {
            return Status(StatusCode::kIoError, "trusted_device_ids must be an array");
        }

        for (nlohmann::json::const_iterator it = device_ids_json.begin(); it != device_ids_json.end(); ++it) {
            if (!it->is_string()) {
                continue;
            }
            trusted_device_ids->push_back(it->get<std::string>());
        }

        return Status::ok_status();
    } catch (const std::exception& exception) {
        return Status(StatusCode::kIoError, exception.what());
    }
}

Status save_trusted_device_ids(const std::string& store_path, const std::vector<std::string>& trusted_device_ids) {
    nlohmann::json store_json;
    store_json["trusted_device_ids"] = trusted_device_ids;

    std::ofstream out_file(store_path.c_str(), std::ios::trunc);
    if (!out_file.is_open()) {
        return Status(StatusCode::kIoError, "Failed opening trusted device store for write");
    }

    out_file << store_json.dump(2) << "\n";
    if (!out_file) {
        return Status(StatusCode::kIoError, "Failed writing trusted device store");
    }

    return Status::ok_status();
}

}  // namespace internal
}  // namespace STManager
