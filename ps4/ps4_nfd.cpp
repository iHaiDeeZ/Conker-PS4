// Conker's Bad Fur Day: Recompiled for PS4: the file dialogs (nativefiledialog-extended's API).
//
// The PS4 has no file dialog for apps. "Load ROM" and the mod installer instead take the files they
// ask for (by extension) from the places a player can put files: the app's folder /data/Conker
// (FTP) and the top folder, or a Conker folder, of a USB drive.

#include "nfd.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern "C" const char *ps4_data_dir(); // port/ps4_crashlog.cpp

namespace {
std::string error;

struct PathSet {
    std::vector<std::string> paths;
};

std::vector<std::filesystem::path> search_folders() {
    std::vector<std::filesystem::path> folders;
    const std::filesystem::path data = ps4_data_dir();
    folders.push_back(data);
    folders.push_back(data / "install");
    for (int usb = 0; usb < 8; usb++) {
        const std::filesystem::path root = "/mnt/usb" + std::to_string(usb);
        folders.push_back(root);
        folders.push_back(root / "Conker");
    }
    return folders;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// "z64,n64,v64" from every filter
std::vector<std::string> extensions(const nfdnfilteritem_t *filters, nfdfiltersize_t count) {
    std::vector<std::string> list;
    for (nfdfiltersize_t i = 0; i < count; i++) {
        std::string spec = filters[i].spec ? filters[i].spec : "";
        size_t start = 0;
        while (start <= spec.size()) {
            const size_t comma = spec.find(',', start);
            std::string ext = spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!ext.empty())
                list.push_back("." + lower(ext));
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
    }
    return list;
}

std::vector<std::string> find_files(const nfdnfilteritem_t *filters, nfdfiltersize_t count) {
    const std::vector<std::string> wanted = extensions(filters, count);
    std::vector<std::string> found;
    for (const auto &folder : search_folders()) {
        std::error_code ec;
        if (!std::filesystem::is_directory(folder, ec))
            continue;
        std::vector<std::string> here;
        for (const auto &entry : std::filesystem::directory_iterator(folder, ec)) {
            if (!entry.is_regular_file(ec))
                continue;
            const std::string ext = lower(entry.path().extension().string());
            if (wanted.empty() || std::find(wanted.begin(), wanted.end(), ext) != wanted.end())
                here.push_back(entry.path().string());
        }
        std::sort(here.begin(), here.end());
        found.insert(found.end(), here.begin(), here.end());
    }
    return found;
}

char *copy(const std::string &s) {
    char *out = static_cast<char *>(std::malloc(s.size() + 1));
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}
} // namespace

extern "C" {

nfdresult_t NFD_Init(void) {
    return NFD_OKAY;
}

void NFD_Quit(void) {}

const char *NFD_GetError(void) {
    return error.empty() ? nullptr : error.c_str();
}

void NFD_ClearError(void) {
    error.clear();
}

void NFD_FreePathN(nfdnchar_t *path) {
    std::free(path);
}

void NFD_FreePathU8(nfdu8char_t *path) {
    std::free(path);
}

nfdresult_t NFD_OpenDialogN(nfdnchar_t **out, const nfdnfilteritem_t *filters, nfdfiltersize_t count, const nfdnchar_t *) {
    const auto files = find_files(filters, count);
    if (files.empty()) {
        error = std::string("No file found: put it in ") + ps4_data_dir() + " or on a USB drive";
        return NFD_ERROR;
    }
    *out = copy(files.front());
    return NFD_OKAY;
}

nfdresult_t NFD_OpenDialogU8(nfdu8char_t **out, const nfdu8filteritem_t *filters, nfdfiltersize_t count, const nfdu8char_t *default_path) {
    return NFD_OpenDialogN(out, filters, count, default_path);
}

nfdresult_t NFD_OpenDialogMultipleN(const nfdpathset_t **out, const nfdnfilteritem_t *filters, nfdfiltersize_t count, const nfdnchar_t *) {
    auto files = find_files(filters, count);
    if (files.empty()) {
        error = std::string("No file found: put it in ") + ps4_data_dir() + "/install or on a USB drive";
        return NFD_ERROR;
    }
    *out = reinterpret_cast<const nfdpathset_t *>(new PathSet{ std::move(files) });
    return NFD_OKAY;
}

nfdresult_t NFD_OpenDialogMultipleU8(const nfdpathset_t **out, const nfdu8filteritem_t *filters, nfdfiltersize_t count, const nfdu8char_t *default_path) {
    return NFD_OpenDialogMultipleN(out, filters, count, default_path);
}

nfdresult_t NFD_SaveDialogN(nfdnchar_t **out, const nfdnfilteritem_t *, nfdfiltersize_t, const nfdnchar_t *, const nfdnchar_t *default_name) {
    *out = copy((std::filesystem::path(ps4_data_dir()) / (default_name ? default_name : "file")).string());
    return NFD_OKAY;
}

nfdresult_t NFD_SaveDialogU8(nfdu8char_t **out, const nfdu8filteritem_t *filters, nfdfiltersize_t count, const nfdu8char_t *default_path, const nfdu8char_t *default_name) {
    return NFD_SaveDialogN(out, filters, count, default_path, default_name);
}

nfdresult_t NFD_PickFolderN(nfdnchar_t **out, const nfdnchar_t *) {
    *out = copy(ps4_data_dir());
    return NFD_OKAY;
}

nfdresult_t NFD_PickFolderU8(nfdu8char_t **out, const nfdu8char_t *default_path) {
    return NFD_PickFolderN(out, default_path);
}

nfdresult_t NFD_PathSet_GetCount(const nfdpathset_t *set, nfdpathsetsize_t *count) {
    *count = static_cast<nfdpathsetsize_t>(reinterpret_cast<const PathSet *>(set)->paths.size());
    return NFD_OKAY;
}

nfdresult_t NFD_PathSet_GetPathN(const nfdpathset_t *set, nfdpathsetsize_t index, nfdnchar_t **out) {
    const auto &paths = reinterpret_cast<const PathSet *>(set)->paths;
    if (index >= paths.size())
        return NFD_ERROR;
    *out = copy(paths[index]);
    return NFD_OKAY;
}

nfdresult_t NFD_PathSet_GetPathU8(const nfdpathset_t *set, nfdpathsetsize_t index, nfdu8char_t **out) {
    return NFD_PathSet_GetPathN(set, index, out);
}

void NFD_PathSet_FreePathN(const nfdnchar_t *path) {
    std::free(const_cast<nfdnchar_t *>(path));
}

void NFD_PathSet_FreePathU8(const nfdu8char_t *path) {
    std::free(const_cast<nfdu8char_t *>(path));
}

void NFD_PathSet_Free(const nfdpathset_t *set) {
    delete reinterpret_cast<const PathSet *>(set);
}

} // extern "C"
