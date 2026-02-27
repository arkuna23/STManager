#ifdef _WIN32
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0601
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
// clang-format on

#include <mutex>
#else
#include <unistd.h>
#endif

#include <fcntl.h>

#include <cctype>
#include <cerrno>
#include <cstring>
#include <sstream>

#include "platform_compat.h"

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

Status current_device_name(std::string* device_name) {
    if (device_name == NULL) {
        return Status(StatusCode::kSyncProtocolError, "device_name output cannot be null");
    }

    device_name->clear();

#ifdef _WIN32
    char host_name_buffer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD host_name_size = static_cast<DWORD>(sizeof(host_name_buffer));
    if (!GetComputerNameA(host_name_buffer, &host_name_size)) {
        std::ostringstream message_stream;
        message_stream << "GetComputerNameA failed: " << static_cast<unsigned long>(GetLastError());
        return Status(StatusCode::kIoError, message_stream.str());
    }

    device_name->assign(host_name_buffer, static_cast<std::string::size_type>(host_name_size));
#else
    char host_name_buffer[256];
    std::memset(host_name_buffer, 0, sizeof(host_name_buffer));
    if (gethostname(host_name_buffer, sizeof(host_name_buffer) - 1) != 0) {
        return Status(StatusCode::kIoError, std::strerror(errno));
    }
    host_name_buffer[sizeof(host_name_buffer) - 1] = '\0';
    *device_name = host_name_buffer;
#endif

    while (!device_name->empty() &&
           std::isspace(static_cast<unsigned char>((*device_name)[0])) != 0) {
        device_name->erase(device_name->begin());
    }
    while (!device_name->empty() &&
           std::isspace(static_cast<unsigned char>((*device_name)[device_name->size() - 1])) != 0) {
        device_name->erase(device_name->end() - 1);
    }

    if (device_name->empty()) {
        return Status(StatusCode::kIoError, "Current device name is empty");
    }

    return Status::ok_status();
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

Status utf8_to_utf16(const std::string& utf8_text, std::wstring* utf16_out) {
    if (utf16_out == NULL) {
        return Status(StatusCode::kSyncProtocolError, "utf16 output cannot be null");
    }

    utf16_out->clear();

#ifdef _WIN32
    if (utf8_text.empty()) {
        return Status::ok_status();
    }

    const int wide_char_length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_text.c_str(), -1, NULL, 0);
    if (wide_char_length <= 0) {
        std::ostringstream message_stream;
        message_stream << "MultiByteToWideChar(CP_UTF8) size query failed: "
                       << static_cast<unsigned long>(GetLastError());
        return Status(StatusCode::kIoError, message_stream.str());
    }

    std::wstring wide_text(static_cast<size_t>(wide_char_length), L'\0');
    const int convert_result = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_text.c_str(),
                                                   -1, &wide_text[0], wide_char_length);
    if (convert_result <= 0) {
        std::ostringstream message_stream;
        message_stream << "MultiByteToWideChar(CP_UTF8) conversion failed: "
                       << static_cast<unsigned long>(GetLastError());
        return Status(StatusCode::kIoError, message_stream.str());
    }

    if (!wide_text.empty() && wide_text[wide_text.size() - 1] == L'\0') {
        wide_text.resize(wide_text.size() - 1);
    }

    *utf16_out = wide_text;
    return Status::ok_status();
#else
    std::wstring wide_text;
    wide_text.reserve(utf8_text.size());
    for (std::string::const_iterator it = utf8_text.begin(); it != utf8_text.end(); ++it) {
        wide_text.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*it)));
    }
    *utf16_out = wide_text;
    return Status::ok_status();
#endif
}

Status local_path_to_utf16(const std::string& local_path, std::wstring* utf16_out) {
    return utf8_to_utf16(local_path, utf16_out);
}

int path_lstat(const char* path, struct stat* path_stat) {
#ifdef _WIN32
    if (path == NULL || path_stat == NULL) {
        errno = EINVAL;
        return -1;
    }

    std::wstring wide_path;
    const Status convert_status = utf8_to_utf16(path, &wide_path);
    if (!convert_status.ok()) {
        errno = EINVAL;
        return -1;
    }

    return wstat(wide_path.c_str(), path_stat);
#else
    return lstat(path, path_stat);
#endif
}

int path_mkdir(const char* path, int mode) {
#ifdef _WIN32
    (void) mode;
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    std::wstring wide_path;
    const Status convert_status = utf8_to_utf16(path, &wide_path);
    if (!convert_status.ok()) {
        errno = EINVAL;
        return -1;
    }

    return _wmkdir(wide_path.c_str());
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
    if (path_mkdir(path_template, 0700) != 0) {
        return NULL;
    }
    return path_template;
#else
    return mkdtemp(path_template);
#endif
}

int path_open_read(const char* path) {
#ifdef _WIN32
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    std::wstring wide_path;
    const Status convert_status = utf8_to_utf16(path, &wide_path);
    if (!convert_status.ok()) {
        errno = EINVAL;
        return -1;
    }

    int flags = O_RDONLY;
#ifdef O_BINARY
    flags |= O_BINARY;
#endif
    return _wopen(wide_path.c_str(), flags);
#else
    return open(path, O_RDONLY);
#endif
}

int path_open_write_trunc(const char* path, int mode) {
#ifdef _WIN32
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    std::wstring wide_path;
    const Status convert_status = utf8_to_utf16(path, &wide_path);
    if (!convert_status.ok()) {
        errno = EINVAL;
        return -1;
    }

    int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_BINARY
    flags |= O_BINARY;
#endif
    return _wopen(wide_path.c_str(), flags, mode);
#else
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
#endif
}

int path_unlink(const char* path) {
#ifdef _WIN32
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    std::wstring wide_path;
    const Status convert_status = utf8_to_utf16(path, &wide_path);
    if (!convert_status.ok()) {
        errno = EINVAL;
        return -1;
    }

    return _wunlink(wide_path.c_str());
#else
    return unlink(path);
#endif
}

int path_rmdir(const char* path) {
#ifdef _WIN32
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    std::wstring wide_path;
    const Status convert_status = utf8_to_utf16(path, &wide_path);
    if (!convert_status.ok()) {
        errno = EINVAL;
        return -1;
    }

    return _wrmdir(wide_path.c_str());
#else
    return rmdir(path);
#endif
}

int path_rename(const char* source_path, const char* destination_path) {
#ifdef _WIN32
    if (source_path == NULL || destination_path == NULL) {
        errno = EINVAL;
        return -1;
    }

    std::wstring source_utf16;
    const Status source_status = utf8_to_utf16(source_path, &source_utf16);
    if (!source_status.ok()) {
        errno = EINVAL;
        return -1;
    }

    std::wstring destination_utf16;
    const Status destination_status = utf8_to_utf16(destination_path, &destination_utf16);
    if (!destination_status.ok()) {
        errno = EINVAL;
        return -1;
    }

    return _wrename(source_utf16.c_str(), destination_utf16.c_str());
#else
    return rename(source_path, destination_path);
#endif
}

bool mode_is_symlink(mode_t mode) {
#ifdef _WIN32
    (void) mode;
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
        NULL, static_cast<DWORD>(error_code), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&raw_message), 0, NULL);
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
