#ifndef STMANAGER_PLATFORM_COMPAT_HPP
#define STMANAGER_PLATFORM_COMPAT_HPP

#include <STManager/data.h>
#include <sys/stat.h>

#include <string>

namespace STManager {
namespace internal {

Status ensure_socket_runtime();
Status current_device_name(std::string* device_name);

int close_file_fd(int fd);
int close_socket_fd(int fd);

Status utf8_to_utf16(const std::string& utf8_text, std::wstring* utf16_out);
Status local_path_to_utf16(const std::string& local_path, std::wstring* utf16_out);

int path_lstat(const char* path, struct stat* path_stat);
int path_mkdir(const char* path, int mode);
char* path_mkdtemp(char* path_template);
int path_open_read(const char* path);
int path_open_write_trunc(const char* path, int mode);
int path_unlink(const char* path);
int path_rmdir(const char* path);
int path_rename(const char* source_path, const char* destination_path);
bool mode_is_symlink(mode_t mode);

bool socket_error_is_interrupt();
std::string socket_last_error_message();

}  // namespace internal
}  // namespace STManager

#endif
