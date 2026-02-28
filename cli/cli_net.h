#ifndef STMANAGER_CLI_NET_HPP
#define STMANAGER_CLI_NET_HPP

#include <string>

namespace STManagerCli {

bool is_wildcard_host(const std::string& host);
bool is_connectable_host(const std::string& host);
std::string runtime_display_host(const std::string& info_host, const std::string& bind_host);

}  // namespace STManagerCli

#endif
