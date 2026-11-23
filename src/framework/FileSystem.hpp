#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
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
    const std::filesystem::path& BasePath() const {
        return base_path_;
    }

    std::filesystem::path RelativePathToOSPath(const std::string& relative_path) const {
        std::filesystem::path p = std::filesystem::u8path(relative_path);
        if (p.is_relative()) {
            p = base_path_ / p;
        }
        return p.lexically_normal();
    }

    // The whole file, as it is on disk. False if it cannot be read.
    bool ReadFile(const std::filesystem::path& path, std::string& text) const {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return false;
        }
        std::ostringstream raw;
        raw << in.rdbuf();
        text = raw.str();
        return true;
    }

    /*
     * Writes `text` as the file at `path`, creating its folders. False if it cannot.
     *
     * NOTE: written next to it first, then put in its place (Sys_ReplaceFile): a program killed halfway through
     * leaves the old file, not half of the new one.
     */
    bool WriteFile(const std::filesystem::path& path, const std::string& text) const {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::filesystem::path temp = path;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.close();
            if (!out) {
                std::filesystem::remove(temp, ec);
                return false;
            }
        }
        if (!Sys_ReplaceFile(temp, path)) {
            std::filesystem::remove(temp, ec);
            return false;
        }
        return true;
    }

private:
    std::filesystem::path base_path_;
};


inline FileSystem fileSystem;
