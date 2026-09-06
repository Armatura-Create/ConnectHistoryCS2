#include "core/util/fs.h"

#include <cerrno>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#endif

namespace ch {
namespace fs {
namespace {

bool MakeOne(const std::string& path) {
#ifdef _WIN32
    const int rc = _mkdir(path.c_str());
#else
    const int rc = mkdir(path.c_str(), 0755);
#endif
    return rc == 0 || errno == EEXIST;
}

bool IsSeparator(char c) { return c == '/' || c == '\\'; }

}  // namespace

bool DirectoryExists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 && (info.st_mode & S_IFDIR) != 0;
}

bool EnsureDirectory(const std::string& path) {
    if (path.empty() || DirectoryExists(path)) return true;

    // Первый разделитель абсолютного пути (и "C:" на Windows) пропускаем:
    // создавать корень не нужно и нельзя.
    size_t start = 0;
    while (start < path.size() && IsSeparator(path[start])) ++start;

    for (size_t i = start; i <= path.size(); ++i) {
        if (i != path.size() && !IsSeparator(path[i])) continue;

        const std::string part = path.substr(0, i);
        if (part.empty() || part.back() == ':') continue;
        if (!DirectoryExists(part) && !MakeOne(part)) return false;
    }

    return DirectoryExists(path);
}

std::string ParentDirectory(const std::string& path) {
    for (size_t i = path.size(); i > 0; --i) {
        if (IsSeparator(path[i - 1])) return path.substr(0, i - 1);
    }
    return std::string();
}

}  // namespace fs
}  // namespace ch
