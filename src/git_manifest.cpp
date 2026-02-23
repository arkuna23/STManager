#include "git_manifest.h"

#include <dirent.h>
#include <nlohmann/json.hpp>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <fstream>

namespace STManager {
namespace internal {
namespace {

bool is_directory(const std::string& path) {
    struct stat path_stat;
    if (stat(path.c_str(), &path_stat) != 0) {
        return false;
    }
    return S_ISDIR(path_stat.st_mode);
}

std::string join_path(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (lhs[lhs.size() - 1] == '/') {
        return lhs + rhs;
    }
    return lhs + "/" + rhs;
}

std::string trim_copy(const std::string& input) {
    std::string::size_type start_index = 0;
    while (start_index < input.size() && std::isspace(static_cast<unsigned char>(input[start_index]))) {
        ++start_index;
    }

    std::string::size_type end_index = input.size();
    while (end_index > start_index && std::isspace(static_cast<unsigned char>(input[end_index - 1]))) {
        --end_index;
    }

    return input.substr(start_index, end_index - start_index);
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool parse_origin_url_from_git_config(const std::string& config_path, std::string* remote_url) {
    std::ifstream config_file(config_path.c_str());
    if (!config_file.is_open()) {
        return false;
    }

    bool in_origin_section = false;
    std::string line;
    while (std::getline(config_file, line)) {
        const std::string trimmed_line = trim_copy(line);
        if (trimmed_line.empty() || starts_with(trimmed_line, "#") || starts_with(trimmed_line, ";")) {
            continue;
        }

        if (starts_with(trimmed_line, "[") && trimmed_line[trimmed_line.size() - 1] == ']') {
            in_origin_section = (trimmed_line == "[remote \"origin\"]");
            continue;
        }

        if (!in_origin_section) {
            continue;
        }

        const std::string key_prefix = "url";
        if (!starts_with(trimmed_line, key_prefix)) {
            continue;
        }

        const std::string::size_type equal_sign_index = trimmed_line.find('=');
        if (equal_sign_index == std::string::npos) {
            continue;
        }

        const std::string key_name = trim_copy(trimmed_line.substr(0, equal_sign_index));
        if (key_name != "url") {
            continue;
        }

        const std::string candidate_url = trim_copy(trimmed_line.substr(equal_sign_index + 1));
        if (!candidate_url.empty()) {
            *remote_url = candidate_url;
            return true;
        }
    }

    return false;
}

}  // namespace

Status collect_git_extension_info(
    const std::string& extensions_path,
    std::vector<GitExtensionInfo>* git_extensions,
    std::set<std::string>* git_extension_names) {
    DIR* extension_directory = opendir(extensions_path.c_str());
    if (extension_directory == NULL) {
        return Status(StatusCode::kIoError, "Unable to read extensions directory");
    }

    struct dirent* extension_entry = NULL;
    while ((extension_entry = readdir(extension_directory)) != NULL) {
        const std::string extension_name = extension_entry->d_name;
        if (extension_name == "." || extension_name == "..") {
            continue;
        }

        const std::string extension_path = join_path(extensions_path, extension_name);
        if (!is_directory(extension_path)) {
            continue;
        }

        const std::string git_config_path = join_path(join_path(extension_path, ".git"), "config");

        std::string remote_url;
        if (!parse_origin_url_from_git_config(git_config_path, &remote_url)) {
            continue;
        }

        GitExtensionInfo git_extension_info;
        git_extension_info.extension_name = extension_name;
        git_extension_info.remote_url = remote_url;
        git_extensions->push_back(git_extension_info);
        git_extension_names->insert(extension_name);
    }

    closedir(extension_directory);

    std::sort(
        git_extensions->begin(),
        git_extensions->end(),
        [](const GitExtensionInfo& lhs, const GitExtensionInfo& rhs) {
            return lhs.extension_name < rhs.extension_name;
        });

    return Status::ok_status();
}

std::string build_git_manifest_json(const std::vector<GitExtensionInfo>& git_extensions) {
    nlohmann::json manifest_json;
    manifest_json["extensions"] = nlohmann::json::array();

    for (std::vector<GitExtensionInfo>::const_iterator it = git_extensions.begin();
         it != git_extensions.end();
         ++it) {
        nlohmann::json extension_json;
        extension_json["extension_name"] = it->extension_name;
        extension_json["remote_url"] = it->remote_url;
        manifest_json["extensions"].push_back(extension_json);
    }

    return manifest_json.dump(2) + "\n";
}

}  // namespace internal
}  // namespace STManager
