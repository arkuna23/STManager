#include "sync_protocol.h"

#include <nlohmann/json.hpp>

namespace STManager {
namespace internal {
namespace {

Status parse_common_response(
    const std::string& message,
    const std::string& expected_type,
    bool* accepted,
    std::string* response_error) {
    try {
        const nlohmann::json response_json = nlohmann::json::parse(message);

        if (!response_json.is_object()) {
            return Status(StatusCode::kSyncProtocolError, "Invalid protocol response object");
        }

        const std::string message_type = response_json.value("type", "");
        if (message_type != expected_type) {
            return Status(StatusCode::kSyncProtocolError, "Unexpected protocol response type");
        }

        *accepted = response_json.value("ok", false);
        *response_error = response_json.value("error", std::string());
        return Status::ok_status();
    } catch (const std::exception& exception) {
        return Status(StatusCode::kSyncProtocolError, exception.what());
    }
}

}  // namespace

std::string build_pair_request_message(
    const std::string& local_device_id,
    const std::string& local_device_name,
    const PairingOptions& options) {
    nlohmann::json request_json;
    request_json["type"] = "pair_request";
    request_json["device_id"] = local_device_id;
    request_json["device_name"] = local_device_name;
    request_json["pairing_code"] = options.pairing_code;
    request_json["remember_device"] = options.remember_device;
    return request_json.dump();
}

std::string build_auth_request_message(
    const std::string& local_device_id,
    const std::string& local_device_name,
    SyncDirection direction) {
    nlohmann::json request_json;
    request_json["type"] = "auth_request";
    request_json["device_id"] = local_device_id;
    request_json["device_name"] = local_device_name;
    request_json["direction"] = direction == SyncDirection::kPush ? "push" : "pull";
    return request_json.dump();
}

Status parse_pair_response_message(const std::string& message, bool* accepted, std::string* response_error) {
    return parse_common_response(message, "pair_response", accepted, response_error);
}

Status parse_auth_response_message(const std::string& message, bool* accepted, std::string* response_error) {
    return parse_common_response(message, "auth_response", accepted, response_error);
}

}  // namespace internal
}  // namespace STManager
