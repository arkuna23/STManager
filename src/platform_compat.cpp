#include "platform_compat.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

#include <direct.h>
#include <io.h>
#include <process.h>
#include <stdio.h>

#include <mutex>
#else
#include <unistd.h>
#endif

#include <cerrno>
#include <cstring>
#include <sstream>

namespace STManager {
namespace internal {

Status ensure_socket_runtime() {
#ifdef _WIN32
    static std::once_flag once_flag;
    static Status init_status = Status::ok_status();
    std::call_once(once_flag, []() {
        WSADATA wsa_data;
        const int startup_result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
        if (startup_result != 0) {
            std::ostringstream message_stream;
            message_stream << "WSAStartup failed: " << startup_result;
            init_status = Status(StatusCode::kIoError, message_stream.str());
        }
    });
    return init_status;
#else
    return Status::ok_status();
#endif
}

int close_file_fd(int fd) {
#ifdef _WIN32
    return _close(fd);
#else
    return close(fd);
#endif
}

int close_socket_fd(int fd) {
#ifdef _WIN32
    return closesocket(static_cast<SOCKET>(fd));
#else
    return close(fd);
#endif
}

int path_lstat(const char* path, struct stat* path_stat) {
#ifdef _WIN32
    return stat(path, path_stat);
#else
    return lstat(path, path_stat);
#endif
}

int path_mkdir(const char* path, int mode) {
#ifdef _WIN32
    (void)mode;
    return _mkdir(path);
#else
    return mkdir(path, static_cast<mode_t>(mode));
#endif
}

char* path_mkdtemp(char* path_template) {
#ifdef _WIN32
    if (path_template == NULL) {
        return NULL;
    }
    if (_mktemp_s(path_template, std::strlen(path_template) + 1) != 0) {
        return NULL;
    }
    if (_mkdir(path_template) != 0) {
        return NULL;
    }
    return path_template;
#else
    return mkdtemp(path_template);
#endif
}

bool mode_is_symlink(mode_t mode) {
#ifdef _WIN32
    (void)mode;
    return false;
#else
    return S_ISLNK(mode);
#endif
}

bool socket_error_is_interrupt() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

std::string socket_last_error_message() {
#ifdef _WIN32
    const int error_code = WSAGetLastError();
    char* raw_message = NULL;
    const DWORD format_result = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        static_cast<DWORD>(error_code),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&raw_message),
        0,
        NULL);
    if (format_result == 0 || raw_message == NULL) {
        std::ostringstream fallback_stream;
        fallback_stream << "WSA error " << error_code;
        return fallback_stream.str();
    }

    std::string message(raw_message);
    LocalFree(raw_message);
    return message;
#else
    return std::strerror(errno);
#endif
}

}  // namespace internal
}  // namespace STManager
