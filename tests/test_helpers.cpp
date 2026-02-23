#include "test_helpers.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace STManagerTest {

std::string join_path(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (lhs[lhs.size() - 1] == '/') {
        return lhs + rhs;
    }
    return lhs + "/" + rhs;
}

bool path_exists(const std::string& path) {
    struct stat path_stat;
    return lstat(path.c_str(), &path_stat) == 0;
}

bool is_directory(const std::string& path) {
    struct stat path_stat;
    if (stat(path.c_str(), &path_stat) != 0) {
        return false;
    }
    return S_ISDIR(path_stat.st_mode);
}

bool create_directories(const std::string& path) {
    if (path.empty()) {
        return false;
    }

    if (is_directory(path)) {
        return true;
    }

    std::string current_path;
    if (path[0] == '/') {
        current_path = "/";
    }

    std::string current_part;
    for (std::string::size_type index = 0; index < path.size(); ++index) {
        const char value = path[index];
        if (value == '/') {
            if (!current_part.empty()) {
                current_path = join_path(current_path, current_part);
                if (!is_directory(current_path)) {
                    if (mkdir(current_path.c_str(), 0755) != 0 && errno != EEXIST) {
                        return false;
                    }
                }
                current_part.clear();
            }
            continue;
        }
        current_part.push_back(value);
    }

    if (!current_part.empty()) {
        current_path = join_path(current_path, current_part);
        if (!is_directory(current_path)) {
            if (mkdir(current_path.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
    }

    return true;
}

bool write_file(const std::string& path, const std::string& content) {
    const std::string::size_type slash_position = path.find_last_of('/');
    if (slash_position != std::string::npos) {
        if (!create_directories(path.substr(0, slash_position))) {
            return false;
        }
    }

    std::ofstream out_file(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out_file.is_open()) {
        return false;
    }

    out_file << content;
    return static_cast<bool>(out_file);
}

std::string read_file(const std::string& path) {
    std::ifstream in_file(path.c_str(), std::ios::binary);
    if (!in_file.is_open()) {
        return std::string();
    }

    std::string content;
    char buffer[4096];
    while (in_file.read(buffer, sizeof(buffer)) || in_file.gcount() > 0) {
        content.append(buffer, static_cast<std::string::size_type>(in_file.gcount()));
    }

    return content;
}

bool copy_file_contents(const std::string& src_path, const std::string& dst_path) {
    std::ifstream src_file(src_path.c_str(), std::ios::binary);
    if (!src_file.is_open()) {
        return false;
    }

    const std::string::size_type slash_position = dst_path.find_last_of('/');
    if (slash_position != std::string::npos) {
        if (!create_directories(dst_path.substr(0, slash_position))) {
            return false;
        }
    }

    std::ofstream dst_file(dst_path.c_str(), std::ios::binary | std::ios::trunc);
    if (!dst_file.is_open()) {
        return false;
    }

    char buffer[4096];
    while (src_file.read(buffer, sizeof(buffer)) || src_file.gcount() > 0) {
        dst_file.write(buffer, src_file.gcount());
    }

    return static_cast<bool>(dst_file);
}

bool copy_directory_recursive(const std::string& src_path, const std::string& dst_path) {
    if (!is_directory(src_path)) {
        return false;
    }

    if (!create_directories(dst_path)) {
        return false;
    }

    DIR* directory = opendir(src_path.c_str());
    if (directory == NULL) {
        return false;
    }

    bool success = true;
    struct dirent* entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }

        const std::string src_child_path = join_path(src_path, name);
        const std::string dst_child_path = join_path(dst_path, name);

        struct stat child_stat;
        if (lstat(src_child_path.c_str(), &child_stat) != 0) {
            success = false;
            break;
        }

        if (S_ISDIR(child_stat.st_mode)) {
            if (!copy_directory_recursive(src_child_path, dst_child_path)) {
                success = false;
                break;
            }
        } else if (S_ISREG(child_stat.st_mode)) {
            if (!copy_file_contents(src_child_path, dst_child_path)) {
                success = false;
                break;
            }
        }
    }

    closedir(directory);
    return success;
}

bool remove_directory_recursive(const std::string& path) {
    struct stat path_stat;
    if (lstat(path.c_str(), &path_stat) != 0) {
        return errno == ENOENT;
    }

    if (S_ISDIR(path_stat.st_mode)) {
        DIR* directory = opendir(path.c_str());
        if (directory == NULL) {
            return false;
        }

        bool success = true;
        struct dirent* entry = NULL;
        while ((entry = readdir(directory)) != NULL) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") {
                continue;
            }

            const std::string child_path = join_path(path, name);
            if (!remove_directory_recursive(child_path)) {
                success = false;
                break;
            }
        }

        closedir(directory);
        if (!success) {
            return false;
        }

        return rmdir(path.c_str()) == 0;
    }

    return unlink(path.c_str()) == 0;
}

std::string create_temp_directory(const std::string& prefix) {
    char template_buffer[PATH_MAX];
    std::snprintf(template_buffer, sizeof(template_buffer), "/tmp/%s-XXXXXX", prefix.c_str());
    char* created_path = mkdtemp(template_buffer);
    if (created_path == NULL) {
        return std::string();
    }

    return std::string(created_path);
}

std::string create_sillytavern_fixture(const std::string& case_name) {
    const std::string fixture_root = create_temp_directory("stmanager-" + case_name);
    if (fixture_root.empty()) {
        return std::string();
    }

    const std::string source_root = "SillyTavern";
    const std::string source_data = join_path(source_root, "data");
    const std::string fixture_data = join_path(fixture_root, "data");

    if (is_directory(source_data)) {
        if (!copy_directory_recursive(source_data, fixture_data)) {
            remove_directory_recursive(fixture_root);
            return std::string();
        }
    } else {
        if (!create_directories(fixture_data)) {
            remove_directory_recursive(fixture_root);
            return std::string();
        }
    }

    if (!create_directories(join_path(fixture_root, "public/scripts/extensions"))) {
        remove_directory_recursive(fixture_root);
        return std::string();
    }

    return fixture_root;
}

}  // namespace STManagerTest
