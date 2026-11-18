#include <engine/core/Content.hpp>

#include "ContentMounts.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <share.h>
#include <shared_mutex>
#include <system_error>
#include <utility>

#include <engine/core/PathUtils.hpp>

namespace engine::content {
namespace {

struct MountPoint {
    std::vector<std::string> roots; // folded generic forms: as given, and canonical when it differs
    std::shared_ptr<const Source> source;
};

std::shared_mutex& MountLock() {
    static std::shared_mutex lock;
    return lock;
}

std::vector<MountPoint>& Mounts() {
    static std::vector<MountPoint> mounts;
    return mounts;
}

std::string Generic(const std::filesystem::path& path) {
    std::error_code ec;
    const auto absolute = path.is_absolute() ? path : std::filesystem::absolute(path, ec);
    auto text = ToGenericUtf8(absolute.lexically_normal());
    while (text.size() > 1 && text.back() == '/' && !text.ends_with(":/")) text.pop_back();
    return text;
}

std::vector<std::string> RootForms(const std::filesystem::path& root) {
    std::vector<std::string> forms{FoldCase(Generic(root))};
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(std::filesystem::absolute(root, ec), ec);
    if (!ec) {
        if (auto form = FoldCase(Generic(canonical)); form != forms.front()) forms.push_back(std::move(form));
    }
    return forms;
}

bool SeekTarget(std::int64_t offset, int origin, std::uint64_t current, std::uint64_t size, std::uint64_t& target) {
    std::int64_t base = 0;
    if (origin == SEEK_CUR) base = static_cast<std::int64_t>(current);
    else if (origin == SEEK_END) base = static_cast<std::int64_t>(size);
    else if (origin != SEEK_SET) return false;
    if (offset < -base || (offset > 0 && static_cast<std::uint64_t>(offset) > size - base)) return false;
    target = static_cast<std::uint64_t>(base + offset);
    return true;
}

class DiskStream final : public Stream {
public:
    DiskStream(std::FILE* file, std::uint64_t size) : file_(file), size_(size) {}
    ~DiskStream() override { std::fclose(file_); }
    std::size_t Read(void* data, std::size_t size) override { return std::fread(data, 1, size, file_); }
    bool Seek(std::int64_t offset, int origin) override {
        std::uint64_t target = 0;
        return SeekTarget(offset, origin, Tell(), size_, target) &&
               _fseeki64(file_, static_cast<std::int64_t>(target), SEEK_SET) == 0;
    }
    std::uint64_t Tell() const override {
        const auto position = _ftelli64(file_);
        return position < 0 ? size_ : static_cast<std::uint64_t>(position);
    }
    std::uint64_t Size() const override { return size_; }

private:
    std::FILE* file_;
    std::uint64_t size_;
};

class MemoryStream final : public Stream {
public:
    MemoryStream(std::span<const std::uint8_t> bytes, std::shared_ptr<const void> owner)
        : bytes_(bytes), owner_(std::move(owner)) {}
    std::size_t Read(void* data, std::size_t size) override {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(size, bytes_.size() - position_));
        if (count != 0) std::memcpy(data, bytes_.data() + position_, count);
        position_ += count;
        return count;
    }
    bool Seek(std::int64_t offset, int origin) override {
        return SeekTarget(offset, origin, position_, bytes_.size(), position_);
    }
    std::uint64_t Tell() const override { return position_; }
    std::uint64_t Size() const override { return bytes_.size(); }

private:
    std::span<const std::uint8_t> bytes_;
    std::shared_ptr<const void> owner_;
    std::uint64_t position_ = 0;
};

} // namespace

namespace detail {

Mounted Find(const std::filesystem::path& path) {
    const std::shared_lock lock(MountLock());
    if (Mounts().empty()) return {};
    const auto text = Generic(path);
    const auto folded = FoldCase(text);
    Mounted best;
    std::size_t bestLength = 0;
    for (const auto& mount : Mounts()) {
        for (const auto& root : mount.roots) {
            if (best.source && root.size() <= bestLength) continue;
            const bool slash = root.ends_with('/');
            if (folded == root) {
                best = {mount.source, {}};
            } else if (folded.starts_with(root) && (slash || folded[root.size()] == '/')) {
                best = {mount.source, text.substr(root.size() + (slash ? 0 : 1))};
            } else {
                continue;
            }
            bestLength = root.size();
        }
    }
    return best;
}

} // namespace detail

std::string FoldCase(std::string_view path) {
    std::string folded(path);
    for (auto& c : folded) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return folded;
}

void Mount(const std::filesystem::path& root, std::shared_ptr<const Source> source) {
    MountPoint mount{RootForms(root), std::move(source)};
    const std::unique_lock lock(MountLock());
    std::erase_if(Mounts(), [&](const MountPoint& m) { return m.roots.front() == mount.roots.front(); });
    Mounts().push_back(std::move(mount));
}

void Unmount(const std::filesystem::path& root) {
    const auto form = FoldCase(Generic(root));
    const std::unique_lock lock(MountLock());
    std::erase_if(Mounts(), [&](const MountPoint& m) { return m.roots.front() == form; });
}

bool IsFile(const std::filesystem::path& file) {
    if (const auto mounted = detail::Find(file); mounted.source && mounted.source->IsFile(mounted.path)) return true;
    std::error_code ec;
    return std::filesystem::is_regular_file(file, ec);
}

bool IsDirectory(const std::filesystem::path& directory) {
    if (const auto mounted = detail::Find(directory); mounted.source && mounted.source->IsDirectory(mounted.path))
        return true;
    std::error_code ec;
    return std::filesystem::is_directory(directory, ec);
}

std::unique_ptr<Stream> Open(const std::filesystem::path& file, std::string& error) {
    if (const auto mounted = detail::Find(file); mounted.source && mounted.source->IsFile(mounted.path)) {
        auto stream = mounted.source->Open(mounted.path, error);
        if (!stream) error = "cannot read " + ToUtf8(file) + ": " + error;
        return stream;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
        error = "cannot read " + ToUtf8(file) + ": " + ec.message();
        return nullptr;
    }
    // Shared like the editor's other readers, so saving a file it streams is never blocked.
    std::FILE* handle = _wfsopen(file.c_str(), L"rb", _SH_DENYNO);
    if (!handle) {
        error = "cannot read " + ToUtf8(file);
        return nullptr;
    }
    return std::make_unique<DiskStream>(handle, size);
}

std::unique_ptr<Stream> OpenMemory(std::span<const std::uint8_t> bytes, std::shared_ptr<const void> owner) {
    return std::make_unique<MemoryStream>(bytes, std::move(owner));
}

} // namespace engine::content
