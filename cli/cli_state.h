#ifndef STMANAGER_CLI_STATE_HPP
#define STMANAGER_CLI_STATE_HPP

#include <string>

namespace STManagerCli {

bool detect_sillytavern_root(const std::string& explicit_root, std::string* root_path, std::string* error_message);

}  // namespace STManagerCli

#endif
