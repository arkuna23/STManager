#include "cli_net.h"

namespace STManagerCli {

bool is_wildcard_host(const std::string& host) {
    return host.empty() || host == "0.0.0.0" || host == "::" || host == "[::]";
}

bool is_connectable_host(const std::string& host) {
    return !is_wildcard_host(host);
}

std::string runtime_display_host(const std::string& info_host, const std::string& bind_host) {
    if (!is_wildcard_host(info_host)) {
        return info_host;
    }
    if (!bind_host.empty()) {
        return bind_host;
    }
    if (!info_host.empty()) {
        return info_host;
    }
    return "0.0.0.0";
}

}  // namespace STManagerCli
