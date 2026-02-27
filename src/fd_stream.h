#ifndef STMANAGER_FD_STREAM_HPP
#define STMANAGER_FD_STREAM_HPP

#include <cstdint>
#include <istream>
#include <ostream>
#include <streambuf>
#include <vector>

namespace STManager {
namespace internal {

class FdOutputStreamBuf : public std::streambuf {
public:
    explicit FdOutputStreamBuf(int file_descriptor);

    uint64_t bytes_written() const;
    bool has_error() const;
    int last_error_number() const;

protected:
    std::streamsize xsputn(const char* data, std::streamsize size) override;
    int overflow(int value) override;
    int sync() override;

private:
    int file_descriptor_;
    uint64_t bytes_written_;
    bool has_error_;
    int last_error_number_;
};

class FdOutputStream : public std::ostream {
public:
    explicit FdOutputStream(int file_descriptor);

    uint64_t bytes_written() const;
    bool has_error() const;
    int last_error_number() const;

private:
    FdOutputStreamBuf buffer_;
};

class FdInputStreamBuf : public std::streambuf {
public:
    explicit FdInputStreamBuf(int file_descriptor);

    bool has_error() const;
    int last_error_number() const;

protected:
    int underflow() override;

private:
    int file_descriptor_;
    bool has_error_;
    int last_error_number_;
    std::vector<char> buffer_;
};

class FdInputStream : public std::istream {
public:
    explicit FdInputStream(int file_descriptor);

    bool has_error() const;
    int last_error_number() const;

private:
    FdInputStreamBuf buffer_;
};

}  // namespace internal
}  // namespace STManager

#endif
