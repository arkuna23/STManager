#include "cli_net.h"

namespace STManagerCli {

bool is_connectable_host(const std::string& host) {
    return !host.empty() && host != "0.0.0.0";
}

}  // namespace STManagerCli
