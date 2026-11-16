#pragma once

#include <filesystem>
#include <string>

#include "sys/Sys.hpp"

/*
    The program's file system. 
    Code names the files it needs by a path relative to the base path 
    ("scripts", "data\knowledge") and gets the full OS path, program finds its files wherever 
    it is copied and whatever the working directory.
    (Sys_DefaultBasePath), when the program starts, before main() and any other thread.
*/
class FileSystem {
public:
    FileSystem() : base_path_(Sys_DefaultBasePath()) {}
    const std::filesystem::path& BasePath() const { return base_path_; }

    std::filesystem::path RelativePathToOSPath(const std::string& relative_path) const {
        std::filesystem::path p = std::filesystem::u8path(relative_path);
        if (p.is_relative()) {
            p = base_path_ / p;
        }
        return p.lexically_normal();
    }

private:
    std::filesystem::path base_path_;
};


inline FileSystem fileSystem;
