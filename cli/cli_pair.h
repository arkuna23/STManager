#ifndef STMANAGER_CLI_PAIR_HPP
#define STMANAGER_CLI_PAIR_HPP

#include <STManager/manager.h>

#include "cli_args.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace STManagerCli {

bool select_pair_device(
    const PairRestoreArgs& pair_args,
    const std::vector<STManager::DeviceInfo>& candidates,
    const STManager::DeviceInfo& auto_selected,
    std::istream& input_stream,
    std::ostream& output_stream,
    std::string* error_message,
    STManager::DeviceInfo* selected_device);

}  // namespace STManagerCli

#endif
