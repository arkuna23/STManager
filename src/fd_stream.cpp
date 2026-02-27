#include "fd_stream.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace STManager {
namespace internal {
namespace {

const size_t kMaxWriteChunkSize = 64 * 1024;
const size_t kMaxReadChunkSize = 64 * 1024;

int write_chunk(int file_descriptor, const char* data, size_t size) {
#ifdef _WIN32
    return _write(file_descriptor, data, static_cast<unsigned int>(size));
#else
    return static_cast<int>(::write(file_descriptor, data, size));
#endif
}

int read_chunk(int file_descriptor, char* data, size_t size) {
#ifdef _WIN32
    return _read(file_descriptor, data, static_cast<unsigned int>(size));
#else
    return static_cast<int>(::read(file_descriptor, data, size));
#endif
}

}  // namespace

FdOutputStreamBuf::FdOutputStreamBuf(int file_descriptor)
    : file_descriptor_(file_descriptor),
      bytes_written_(0),
      has_error_(false),
      last_error_number_(0) {}

uint64_t FdOutputStreamBuf::bytes_written() const {
    return bytes_written_;
}

bool FdOutputStreamBuf::has_error() const {
    return has_error_;
}

int FdOutputStreamBuf::last_error_number() const {
    return last_error_number_;
}

std::streamsize FdOutputStreamBuf::xsputn(const char* data, std::streamsize size) {
    if (size <= 0) {
        return 0;
    }
    if (file_descriptor_ < 0) {
        has_error_ = true;
        last_error_number_ = EBADF;
        return 0;
    }

    size_t written_total_size = 0;
    const size_t requested_size = static_cast<size_t>(size);
    while (written_total_size < requested_size) {
        const size_t remaining_size = requested_size - written_total_size;
        const size_t chunk_size = std::min(remaining_size, kMaxWriteChunkSize);
        const int write_result =
            write_chunk(file_descriptor_, data + written_total_size, chunk_size);
        if (write_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            has_error_ = true;
            last_error_number_ = errno;
            break;
        }
        if (write_result == 0) {
            has_error_ = true;
            last_error_number_ = EIO;
            break;
        }

        written_total_size += static_cast<size_t>(write_result);
        bytes_written_ += static_cast<uint64_t>(write_result);
    }

    return static_cast<std::streamsize>(written_total_size);
}

int FdOutputStreamBuf::overflow(int value) {
    if (traits_type::eq_int_type(value, traits_type::eof())) {
        return traits_type::not_eof(value);
    }

    const char character = static_cast<char>(value);
    const std::streamsize write_size = xsputn(&character, 1);
    if (write_size == 1) {
        return value;
    }
    return traits_type::eof();
}

int FdOutputStreamBuf::sync() {
    return has_error_ ? -1 : 0;
}

FdOutputStream::FdOutputStream(int file_descriptor)
    : std::ostream(NULL),
      buffer_(file_descriptor) {
    rdbuf(&buffer_);
}

uint64_t FdOutputStream::bytes_written() const {
    return buffer_.bytes_written();
}

bool FdOutputStream::has_error() const {
    return buffer_.has_error();
}

int FdOutputStream::last_error_number() const {
    return buffer_.last_error_number();
}

FdInputStreamBuf::FdInputStreamBuf(int file_descriptor)
    : file_descriptor_(file_descriptor),
      has_error_(false),
      last_error_number_(0),
      buffer_(kMaxReadChunkSize) {
    if (!buffer_.empty()) {
        setg(&buffer_[0], &buffer_[0], &buffer_[0]);
    } else {
        setg(NULL, NULL, NULL);
    }
}

bool FdInputStreamBuf::has_error() const {
    return has_error_;
}

int FdInputStreamBuf::last_error_number() const {
    return last_error_number_;
}

int FdInputStreamBuf::underflow() {
    if (gptr() != NULL && gptr() < egptr()) {
        return traits_type::to_int_type(*gptr());
    }

    if (file_descriptor_ < 0) {
        has_error_ = true;
        last_error_number_ = EBADF;
        return traits_type::eof();
    }

    while (true) {
        const int read_result = read_chunk(file_descriptor_, &buffer_[0], buffer_.size());
        if (read_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            has_error_ = true;
            last_error_number_ = errno;
            return traits_type::eof();
        }
        if (read_result == 0) {
            return traits_type::eof();
        }

        setg(&buffer_[0], &buffer_[0], &buffer_[0] + read_result);
        return traits_type::to_int_type(*gptr());
    }
}

FdInputStream::FdInputStream(int file_descriptor)
    : std::istream(NULL),
      buffer_(file_descriptor) {
    rdbuf(&buffer_);
}

bool FdInputStream::has_error() const {
    return buffer_.has_error();
}

int FdInputStream::last_error_number() const {
    return buffer_.last_error_number();
}

}  // namespace internal
}  // namespace STManager
