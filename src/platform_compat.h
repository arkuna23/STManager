#ifndef STMANAGER_PLATFORM_COMPAT_HPP
#define STMANAGER_PLATFORM_COMPAT_HPP

#include <STManager/data.h>

#include <sys/stat.h>

#include <string>

namespace STManager {
namespace internal {

Status ensure_socket_runtime();

int close_file_fd(int fd);
int close_socket_fd(int fd);

int path_lstat(const char* path, struct stat* path_stat);
int path_mkdir(const char* path, int mode);
char* path_mkdtemp(char* path_template);
bool mode_is_symlink(mode_t mode);

bool socket_error_is_interrupt();
std::string socket_last_error_message();

}  // namespace internal
}  // namespace STManager

#endif
