#include <STUtils/data.h>

#include <string>

using namespace std;
using DataMgr = STUtils::DataManager;

DataMgr::DataManager(string path) {
    this->path = path;
}

// TODO
DataMgr DataMgr::locate(const std::string& root) {}
