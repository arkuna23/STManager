#ifndef STMANAGER_ARCHIVE_STREAM_HPP
#define STMANAGER_ARCHIVE_STREAM_HPP

#include <STManager/data.h>

#include <istream>
#include <ostream>

namespace STManager {
namespace internal {

Status write_backup_archive(
    const std::string& data_path,
    const std::string& extensions_path,
    std::ostream& out,
    const BackupOptions& options);

Status validate_backup_archive(std::istream& in);

Status restore_backup_archive(std::istream& in, const std::string& destination_root);

}  // namespace internal
}  // namespace STManager

#endif
