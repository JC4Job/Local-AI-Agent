#pragma once

#include <filesystem>
#include <string>

#include "sys/Sys.hpp"

/*
 * FileSystem - the program's file system.
 *
 * Code names the files it needs by a path relative to the base path ("scripts", "data\knowledge") and gets the
 * full OS path, so the program finds its files wherever it is copied and whatever the working directory.
 *
 * NOTE: the base path (Sys_DefaultBasePath) is found once, when the program starts: `fileSystem` is a global,
 * built before main() and before any other thread, and never changed after.
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
