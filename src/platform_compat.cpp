#include "platform_compat.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <direct.h>
#include <io.h>
#include <process.h>
#include <stdio.h>

#include <mutex>
#else
#include <unistd.h>
#endif

#include <cerrno>
#include <cctype>
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

    while (!device_name->empty() && std::isspace(static_cast<unsigned char>((*device_name)[0])) != 0) {
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

Status local_path_to_utf16(const std::string& local_path, std::wstring* utf16_out) {
    if (utf16_out == NULL) {
        return Status(StatusCode::kSyncProtocolError, "utf16 output cannot be null");
    }

    utf16_out->clear();

#ifdef _WIN32
    if (local_path.empty()) {
        return Status::ok_status();
    }

    const int wide_char_length = MultiByteToWideChar(
        CP_ACP,
        0,
        local_path.c_str(),
        -1,
        NULL,
        0);
    if (wide_char_length <= 0) {
        std::ostringstream message_stream;
        message_stream << "MultiByteToWideChar size query failed: "
                       << static_cast<unsigned long>(GetLastError());
        return Status(StatusCode::kIoError, message_stream.str());
    }

    std::wstring wide_path(static_cast<size_t>(wide_char_length), L'\0');
    const int convert_result = MultiByteToWideChar(
        CP_ACP,
        0,
        local_path.c_str(),
        -1,
        &wide_path[0],
        wide_char_length);
    if (convert_result <= 0) {
        std::ostringstream message_stream;
        message_stream << "MultiByteToWideChar conversion failed: "
                       << static_cast<unsigned long>(GetLastError());
        return Status(StatusCode::kIoError, message_stream.str());
    }

    if (!wide_path.empty() && wide_path[wide_path.size() - 1] == L'\0') {
        wide_path.resize(wide_path.size() - 1);
    }

    *utf16_out = wide_path;
    return Status::ok_status();
#else
    std::wstring wide_path;
    wide_path.reserve(local_path.size());
    for (std::string::const_iterator it = local_path.begin(); it != local_path.end(); ++it) {
        wide_path.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*it)));
    }
    *utf16_out = wide_path;
    return Status::ok_status();
#endif
}

}  // namespace internal
}  // namespace STManager
