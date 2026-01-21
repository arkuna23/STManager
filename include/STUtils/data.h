#ifndef STUTILS_DATA_MANAGER_HPP
#define STUTILS_DATA_MANAGER_HPP

#include <STUtils/stutils_export.h>

#include <string>

namespace STUtils {
class DataManager {
public:
    std::string path;

    DataManager(std::string path);

    static DataManager locate(const std::string& root);
};
}  // namespace STUtils

#endif
