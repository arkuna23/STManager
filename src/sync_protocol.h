#ifndef STMANAGER_SYNC_PROTOCOL_HPP
#define STMANAGER_SYNC_PROTOCOL_HPP

#include <STManager/data.h>
#include <STManager/sync.h>

#include <string>

namespace STManager {
namespace internal {

std::string build_pair_request_message(const std::string& local_device_id, const PairingOptions& options);
std::string build_auth_request_message(const std::string& local_device_id, SyncDirection direction);

Status parse_pair_response_message(const std::string& message, bool* accepted, std::string* response_error);
Status parse_auth_response_message(const std::string& message, bool* accepted, std::string* response_error);

}  // namespace internal
}  // namespace STManager

#endif
