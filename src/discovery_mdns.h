#ifndef STMANAGER_DISCOVERY_MDNS_HPP
#define STMANAGER_DISCOVERY_MDNS_HPP

#include <STManager/data.h>
#include <STManager/sync.h>

#include <string>
#include <vector>

namespace STManager {
namespace internal {

Status list_mdns_devices(std::vector<DeviceInfo>* devices);
Status start_discovery_responder(const DeviceInfo& local_device);
Status stop_discovery_responder();

}  // namespace internal
}  // namespace STManager

#endif
