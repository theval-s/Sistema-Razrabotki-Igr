#pragma once

#include <engine/core/Content.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// An in-memory pack: files by path relative to where it is mounted.
class MemorySource final : public engine::content::Source {
public:
    void Add(std::string_view path, std::string_view bytes) {
        files_[engine::content::FoldCase(path)] = std::make_shared<const std::vector<std::uint8_t>>(bytes.begin(), bytes.end());
    }
    bool IsFile(std::string_view path) const override { return files_.contains(engine::content::FoldCase(path)); }
    bool IsDirectory(std::string_view path) const override {
        if (path.empty()) return true;
        const auto prefix = engine::content::FoldCase(path) + "/";
        const auto it = files_.lower_bound(prefix);
        return it != files_.end() && it->first.starts_with(prefix);
    }
    bool Read(std::string_view path, std::vector<std::uint8_t>& data, std::string& error,
              std::uint64_t maxSize) const override {
        const auto* file = Find(path, error);
        if (file && (*file)->size() > maxSize) error = "over the size limit";
        if (!file || (*file)->size() > maxSize) return false;
        data = **file;
        return true;
    }
    std::unique_ptr<engine::content::Stream> Open(std::string_view path, std::string& error) const override {
        const auto* file = Find(path, error);
        return file ? engine::content::OpenMemory(**file, *file) : nullptr;
    }

private:
    using Bytes = std::shared_ptr<const std::vector<std::uint8_t>>;
    const Bytes* Find(std::string_view path, std::string& error) const {
        const auto it = files_.find(engine::content::FoldCase(path));
        if (it == files_.end()) error = "not in the pack";
        return it == files_.end() ? nullptr : &it->second;
    }
    std::map<std::string, Bytes, std::less<>> files_;
};

struct MountScope {
    std::filesystem::path root;
    MountScope(std::filesystem::path where, std::shared_ptr<const engine::content::Source> source) : root(std::move(where)) {
        engine::content::Mount(root, std::move(source));
    }
    ~MountScope() { engine::content::Unmount(root); }
    MountScope(const MountScope&) = delete;
    MountScope& operator=(const MountScope&) = delete;
};
