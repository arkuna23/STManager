#include <STManager/data.h>

#include <string>

using namespace std;
using DataMgr = STManager::DataManager;

DataMgr::DataManager(string path) {
    this->path = path;
}

DataMgr DataMgr::locate(const std::string& root) {}
