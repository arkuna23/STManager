#ifndef STMANAGER_DATA_MANAGER_HPP
#define STMANAGER_DATA_MANAGER_HPP

#include <STManager/stmanager_export.h>

#include <string>

namespace STManager {
class DataManager {
public:
    std::string path;

    DataManager(std::string path);

    static DataManager locate(const std::string& root);
};
}  // namespace STManager

#endif
