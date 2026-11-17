#include <engine/core/Pak.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <zlib.h>

#include <engine/core/FileSystem.hpp>
#include <engine/core/PathUtils.hpp>

namespace engine {
namespace {

constexpr std::uint32_t LocalSignature = 0x04034b50;
constexpr std::uint32_t CentralSignature = 0x02014b50;
constexpr std::uint32_t EndSignature = 0x06054b50;
constexpr std::uint32_t End64Signature = 0x06064b50;
constexpr std::uint32_t Locator64Signature = 0x07064b50;
constexpr std::uint16_t Zip64ExtraId = 0x0001;
constexpr std::uint16_t Utf8Names = 0x0800;
constexpr std::uint16_t Stored = 0;
constexpr std::uint16_t Deflated = 8;
constexpr std::uint16_t Version = 20;
constexpr std::uint16_t Version64 = 45;
constexpr std::uint16_t DosDate = (1 << 5) | 1; // 1980-01-01, the earliest date a zip can hold
constexpr std::uint64_t Max16 = 0xFFFF;
constexpr std::uint64_t Max32 = 0xFFFFFFFF;
constexpr std::size_t LocalHeaderSize = 30;
constexpr std::size_t CentralHeaderSize = 46;
constexpr std::size_t EndSize = 22;
constexpr std::size_t ZlibChunk = std::size_t{1} << 30; // zlib counts bytes in 32 bits

[[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }

std::uint64_t Field(std::span<const std::uint8_t> bytes, std::uint64_t offset, int size) {
    if (offset > bytes.size() || static_cast<std::uint64_t>(size) > bytes.size() - offset)
        Fail("pack is truncated or damaged");
    std::uint64_t value = 0;
    for (int i = size - 1; i >= 0; --i) value = (value << 8) | bytes[static_cast<std::size_t>(offset) + i];
    return value;
}
std::uint64_t U16(std::span<const std::uint8_t> bytes, std::uint64_t offset) { return Field(bytes, offset, 2); }
std::uint64_t U32(std::span<const std::uint8_t> bytes, std::uint64_t offset) { return Field(bytes, offset, 4); }
std::uint64_t U64(std::span<const std::uint8_t> bytes, std::uint64_t offset) { return Field(bytes, offset, 8); }

struct Out {
    std::vector<std::uint8_t> data;
    void Put(std::uint64_t value, int size) {
        for (int i = 0; i < size; ++i) data.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    void U16(std::uint64_t value) { Put(value, 2); }
    void U32(std::uint64_t value) { Put(value, 4); }
    void U64(std::uint64_t value) { Put(value, 8); }
    void Text(std::string_view text) { data.insert(data.end(), text.begin(), text.end()); }
};

bool ValidName(std::string_view name) {
    if (name.empty() || name.find_first_of(std::string_view("\\:\0", 3)) != std::string_view::npos) return false;
    for (std::size_t start = 0; start <= name.size();) {
        auto end = name.find('/', start);
        if (end == std::string_view::npos) end = name.size();
        const auto part = name.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = end + 1;
    }
    return true;
}

std::uint32_t Crc(std::span<const std::uint8_t> bytes) {
    return static_cast<std::uint32_t>(crc32_z(0, bytes.data(), bytes.size()));
}

/// Gives up once the output reaches `limit`, so incompressible files cost little.
std::optional<std::vector<std::uint8_t>> Deflate(std::span<const std::uint8_t> data, std::size_t limit) {
    z_stream z{};
    if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        Fail("cannot start deflate");
    struct End { z_stream& z; ~End() { deflateEnd(&z); } } end{z};
    std::vector<std::uint8_t> out;
    std::array<Bytef, 1 << 16> buffer{};
    const Bytef* next = data.data();
    std::size_t left = data.size();
    int result = Z_OK;
    do {
        const auto chunk = static_cast<uInt>(std::min(left, ZlibChunk));
        z.next_in = const_cast<Bytef*>(next);
        z.avail_in = chunk;
        const int flush = chunk == left ? Z_FINISH : Z_NO_FLUSH;
        do {
            z.next_out = buffer.data();
            z.avail_out = static_cast<uInt>(buffer.size());
            result = deflate(&z, flush);
            if (result == Z_STREAM_ERROR) Fail("deflate failed");
            out.insert(out.end(), buffer.data(), buffer.data() + (buffer.size() - z.avail_out));
            if (out.size() >= limit) return std::nullopt;
        } while (z.avail_out == 0);
        next += chunk;
        left -= chunk;
    } while (result != Z_STREAM_END);
    return out;
}

std::vector<std::uint8_t> Inflate(std::span<const std::uint8_t> packed, std::uint64_t size) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) Fail("cannot start inflate");
    struct End { z_stream& z; ~End() { inflateEnd(&z); } } end{z};
    Bytef spare = 0;
    const Bytef* in = packed.data();
    std::size_t inLeft = packed.size();
    Bytef* output = out.empty() ? &spare : out.data();
    std::size_t outLeft = out.size();
    for (;;) {
        const auto inChunk = static_cast<uInt>(std::min(inLeft, ZlibChunk));
        const auto outChunk = static_cast<uInt>(std::min(outLeft, ZlibChunk));
        z.next_in = const_cast<Bytef*>(in);
        z.avail_in = inChunk;
        z.next_out = output;
        z.avail_out = outChunk;
        const int result = inflate(&z, Z_NO_FLUSH);
        const std::size_t used = inChunk - z.avail_in, written = outChunk - z.avail_out;
        in += used;
        inLeft -= used;
        output += written;
        outLeft -= written;
        if (result == Z_STREAM_END) break;
        if ((result != Z_OK && result != Z_BUF_ERROR) || (used == 0 && written == 0))
            Fail("compressed data is damaged");
    }
    if (outLeft != 0) Fail("compressed data is truncated");
    return out;
}

struct Mapping {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE view = nullptr;
    const std::uint8_t* data = nullptr;
    std::uint64_t size = 0;
    Mapping() = default;
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    ~Mapping() {
        if (data) UnmapViewOfFile(data);
        if (view) CloseHandle(view);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    std::span<const std::uint8_t> Bytes() const { return {data, static_cast<std::size_t>(size)}; }
};

std::string LastError() {
    return std::system_category().message(static_cast<int>(GetLastError()));
}

struct PakEntry {
    std::string path;
    std::uint64_t local = 0;
    std::uint64_t packed = 0;
    std::uint64_t size = 0;
    std::uint32_t crc = 0;
    std::uint16_t method = Stored;
};

} // namespace

struct PakSource::Impl {
    std::shared_ptr<const Mapping> mapping;
    std::unordered_map<std::string, PakEntry> files; // by folded path
    std::unordered_set<std::string> directories;      // folded, without the trailing slash

    void Index() {
        const auto bytes = mapping->Bytes();
        if (bytes.size() < EndSize) Fail("not a .sripak (too short)");
        // The end record is the last signature within the longest possible archive comment.
        std::uint64_t end = bytes.size() - EndSize;
        const std::uint64_t lowest = end > Max16 ? end - Max16 : 0;
        while (U32(bytes, end) != EndSignature) {
            if (end == lowest) Fail("not a .sripak (no zip directory)");
            --end;
        }
        std::uint64_t count = U16(bytes, end + 10);
        std::uint64_t directorySize = U32(bytes, end + 12);
        std::uint64_t directoryOffset = U32(bytes, end + 16);
        if (end >= 20 && U32(bytes, end - 20) == Locator64Signature) {
            const auto record = U64(bytes, end - 20 + 8);
            if (U32(bytes, record) != End64Signature) Fail("pack has a damaged zip64 directory record");
            if (U32(bytes, record + 16) != 0 || U32(bytes, record + 20) != 0) Fail("multi-part packs are not supported");
            count = U64(bytes, record + 32);
            directorySize = U64(bytes, record + 40);
            directoryOffset = U64(bytes, record + 48);
        } else if (count == Max16 || directorySize == Max32 || directoryOffset == Max32) {
            Fail("pack is missing its zip64 directory record");
        } else if (U16(bytes, end + 4) != 0 || U16(bytes, end + 6) != 0 || U16(bytes, end + 8) != count) {
            Fail("multi-part packs are not supported");
        }
        if (directoryOffset > end || directorySize > end - directoryOffset ||
            count > directorySize / CentralHeaderSize) {
            Fail("pack directory is damaged");
        }

        const std::uint64_t limit = directoryOffset + directorySize;
        std::uint64_t at = directoryOffset;
        files.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t i = 0; i < count; ++i) {
            if (CentralHeaderSize > limit - at || U32(bytes, at) != CentralSignature) Fail("pack directory is damaged");
            const auto flags = U16(bytes, at + 8);
            const auto method = U16(bytes, at + 10);
            PakEntry entry;
            entry.crc = static_cast<std::uint32_t>(U32(bytes, at + 16));
            entry.packed = U32(bytes, at + 20);
            entry.size = U32(bytes, at + 24);
            const auto nameLength = U16(bytes, at + 28);
            const auto extraLength = U16(bytes, at + 30);
            const auto commentLength = U16(bytes, at + 32);
            std::uint64_t disk = U16(bytes, at + 34);
            entry.local = U32(bytes, at + 42);
            const auto record = CentralHeaderSize + nameLength + extraLength + commentLength;
            if (record > limit - at) Fail("pack directory is damaged");
            const auto name = at + CentralHeaderSize;
            entry.path.assign(reinterpret_cast<const char*>(bytes.data() + name), static_cast<std::size_t>(nameLength));
            for (std::uint64_t extra = name + nameLength, extraEnd = extra + extraLength; extra + 4 <= extraEnd;) {
                const auto id = U16(bytes, extra);
                const auto length = U16(bytes, extra + 2);
                extra += 4;
                if (length > extraEnd - extra) Fail("pack directory is damaged");
                if (id == Zip64ExtraId) {
                    std::uint64_t field = extra;
                    const auto next = [&](int size) {
                        if (static_cast<std::uint64_t>(size) > extra + length - field) Fail("pack has a damaged zip64 field");
                        const auto value = Field(bytes, field, size);
                        field += size;
                        return value;
                    };
                    if (entry.size == Max32) entry.size = next(8);
                    if (entry.packed == Max32) entry.packed = next(8);
                    if (entry.local == Max32) entry.local = next(8);
                    if (disk == Max16) disk = next(4);
                }
                extra += length;
            }
            at += record;

            if (disk != 0) Fail("multi-part packs are not supported");
            if (flags & 1) Fail(entry.path + " is encrypted");
            const bool directory = entry.path.ends_with('/');
            if (directory) entry.path.pop_back();
            if (!ValidName(entry.path)) Fail("pack has an unsafe entry name: " + entry.path);
            auto key = content::FoldCase(entry.path);
            for (auto slash = key.find('/'); slash != std::string::npos; slash = key.find('/', slash + 1))
                directories.insert(key.substr(0, slash));
            if (directory) {
                directories.insert(std::move(key));
                continue;
            }
            if (method != Stored && method != Deflated) Fail(entry.path + " uses an unsupported compression method");
            if (method == Stored && entry.packed != entry.size) Fail(entry.path + " is damaged");
            if (entry.local >= directoryOffset || entry.packed > directoryOffset - entry.local)
                Fail(entry.path + " lies outside the pack data");
            entry.method = static_cast<std::uint16_t>(method);
            if (!files.emplace(std::move(key), entry).second) Fail("pack has two files named " + entry.path);
        }
        for (const auto& directory : directories) {
            if (files.contains(directory)) Fail("pack has a file and a folder named " + files.at(directory).path);
        }
    }

    const PakEntry* Find(std::string_view path, std::string& error) const {
        const auto it = files.find(content::FoldCase(path));
        if (it == files.end()) error = "not in the pack";
        return it == files.end() ? nullptr : &it->second;
    }

    std::span<const std::uint8_t> Data(const PakEntry& entry) const {
        const auto bytes = mapping->Bytes();
        if (U32(bytes, entry.local) != LocalSignature) Fail("damaged local header");
        const auto start = entry.local + LocalHeaderSize + U16(bytes, entry.local + 26) + U16(bytes, entry.local + 28);
        if (start > bytes.size() || entry.packed > bytes.size() - start) Fail("data is truncated");
        return bytes.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(entry.packed));
    }

    std::vector<std::uint8_t> Extract(const PakEntry& entry) const {
        const auto data = Data(entry);
        auto result = entry.method == Stored ? std::vector<std::uint8_t>(data.begin(), data.end())
                                             : Inflate(data, entry.size);
        if (Crc(result) != entry.crc) Fail("damaged (CRC mismatch)");
        return result;
    }
};

PakSource::PakSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PakSource::~PakSource() = default;

std::shared_ptr<PakSource> PakSource::Load(const std::filesystem::path& file, std::string& error) {
    try {
        auto mapping = std::make_shared<Mapping>();
        mapping->file = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (mapping->file == INVALID_HANDLE_VALUE) Fail(LastError());
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(mapping->file, &size)) Fail(LastError());
        mapping->size = static_cast<std::uint64_t>(size.QuadPart);
        if (mapping->size < EndSize) Fail("not a .sripak (too short)");
        mapping->view = CreateFileMappingW(mapping->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping->view) Fail(LastError());
        mapping->data = static_cast<const std::uint8_t*>(MapViewOfFile(mapping->view, FILE_MAP_READ, 0, 0, 0));
        if (!mapping->data) Fail(LastError());
        auto impl = std::make_unique<Impl>();
        impl->mapping = std::move(mapping);
        impl->Index();
        error.clear();
        return std::shared_ptr<PakSource>(new PakSource(std::move(impl)));
    } catch (const std::exception& e) {
        error = "cannot open " + ToUtf8(file) + ": " + e.what();
        return nullptr;
    }
}

bool PakSource::IsFile(std::string_view path) const {
    return impl_->files.contains(content::FoldCase(path));
}

bool PakSource::IsDirectory(std::string_view path) const {
    return path.empty() || impl_->directories.contains(content::FoldCase(path));
}

bool PakSource::Read(std::string_view path, std::vector<std::uint8_t>& data, std::string& error,
                     std::uint64_t maxSize) const {
    const auto* entry = impl_->Find(path, error);
    if (!entry) return false;
    if (entry->size > maxSize) {
        error = std::to_string(entry->size) + " bytes is over the limit of " + std::to_string(maxSize);
        return false;
    }
    try {
        data = impl_->Extract(*entry);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

std::unique_ptr<content::Stream> PakSource::Open(std::string_view path, std::string& error) const {
    const auto* entry = impl_->Find(path, error);
    if (!entry) return nullptr;
    try {
        if (entry->method == Stored) return content::OpenMemory(impl_->Data(*entry), impl_->mapping);
        auto bytes = std::make_shared<const std::vector<std::uint8_t>>(impl_->Extract(*entry));
        return content::OpenMemory(*bytes, bytes);
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

std::vector<std::string> PakSource::Files() const {
    std::vector<std::string> files;
    files.reserve(impl_->files.size());
    for (const auto& [key, entry] : impl_->files) files.push_back(entry.path);
    std::ranges::sort(files);
    return files;
}

void PakWriter::AddFile(std::string path, std::filesystem::path source, bool compress) {
    entries_.push_back({std::move(path), std::move(source), {}, compress});
}

void PakWriter::AddBytes(std::string path, std::vector<std::uint8_t> bytes, bool compress) {
    entries_.push_back({std::move(path), {}, std::move(bytes), compress});
}

bool PakWriter::Write(const std::filesystem::path& file, std::string& error) const {
    std::vector<const Entry*> sorted;
    std::unordered_set<std::string> names;
    for (const auto& entry : entries_) {
        if (!ValidName(entry.path) || entry.path.size() > Max16) {
            error = "invalid path in pack: " + entry.path;
            return false;
        }
        if (!names.insert(content::FoldCase(entry.path)).second) {
            error = "two files in the pack are named " + entry.path;
            return false;
        }
        sorted.push_back(&entry);
    }
    std::ranges::sort(sorted, {}, &Entry::path);

    thread_local std::mt19937_64 random{std::random_device{}()};
    auto temporary = file;
    temporary += ".tmp." + std::to_string(random());
    try {
        struct Record {
            const Entry* entry;
            std::uint64_t local, packed, size;
            std::uint32_t crc;
            std::uint16_t method;
        };
        std::vector<Record> records;
        records.reserve(sorted.size());
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) Fail("cannot write " + ToUtf8(temporary));
        std::uint64_t offset = 0;
        const auto put = [&](std::span<const std::uint8_t> bytes) {
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            offset += bytes.size();
        };

        for (const auto* entry : sorted) {
            std::vector<std::uint8_t> loaded;
            if (!entry->source.empty()) {
                std::string readError;
                if (!ReadBinaryFile(entry->source, loaded, readError)) Fail(readError);
            }
            const auto& contents = entry->source.empty() ? entry->bytes : loaded;
            std::optional<std::vector<std::uint8_t>> deflated;
            if (entry->compress && contents.size() >= 64) deflated = Deflate(contents, contents.size() - contents.size() / 16);
            const std::span<const std::uint8_t> data = deflated ? std::span<const std::uint8_t>(*deflated) : contents;
            const Record record{entry, offset, data.size(), contents.size(), Crc(contents), deflated ? Deflated : Stored};
            const bool sizes64 = forceZip64_ || record.size >= Max32 || record.packed >= Max32;

            Out header;
            header.U32(LocalSignature);
            header.U16(sizes64 ? Version64 : Version);
            header.U16(Utf8Names);
            header.U16(record.method);
            header.U16(0);
            header.U16(DosDate);
            header.U32(record.crc);
            header.U32(sizes64 ? Max32 : record.packed);
            header.U32(sizes64 ? Max32 : record.size);
            header.U16(entry->path.size());
            header.U16(sizes64 ? 20 : 0);
            header.Text(entry->path);
            if (sizes64) {
                header.U16(Zip64ExtraId);
                header.U16(16);
                header.U64(record.size);
                header.U64(record.packed);
            }
            put(header.data);
            put(data);
            records.push_back(record);
        }

        const std::uint64_t directoryOffset = offset;
        for (const auto& record : records) {
            const bool sizes64 = forceZip64_ || record.size >= Max32 || record.packed >= Max32;
            const bool offset64 = forceZip64_ || record.local >= Max32;
            Out extra;
            if (sizes64 || offset64) {
                extra.U16(Zip64ExtraId);
                extra.U16((sizes64 ? 16 : 0) + (offset64 ? 8 : 0));
                if (sizes64) {
                    extra.U64(record.size);
                    extra.U64(record.packed);
                }
                if (offset64) extra.U64(record.local);
            }
            Out header;
            header.U32(CentralSignature);
            header.U16(Version64);
            header.U16(sizes64 || offset64 ? Version64 : Version);
            header.U16(Utf8Names);
            header.U16(record.method);
            header.U16(0);
            header.U16(DosDate);
            header.U32(record.crc);
            header.U32(sizes64 ? Max32 : record.packed);
            header.U32(sizes64 ? Max32 : record.size);
            header.U16(record.entry->path.size());
            header.U16(extra.data.size());
            header.U16(0); // comment
            header.U16(0); // disk
            header.U16(0); // internal attributes
            header.U32(0); // external attributes
            header.U32(offset64 ? Max32 : record.local);
            header.Text(record.entry->path);
            header.data.insert(header.data.end(), extra.data.begin(), extra.data.end());
            put(header.data);
        }

        const std::uint64_t directorySize = offset - directoryOffset;
        const bool end64 = forceZip64_ || records.size() >= Max16 || directorySize >= Max32 || directoryOffset >= Max32;
        Out end;
        if (end64) {
            const auto record = offset;
            end.U32(End64Signature);
            end.U64(44);
            end.U16(Version64);
            end.U16(Version64);
            end.U32(0);
            end.U32(0);
            end.U64(records.size());
            end.U64(records.size());
            end.U64(directorySize);
            end.U64(directoryOffset);
            end.U32(Locator64Signature);
            end.U32(0);
            end.U64(record);
            end.U32(1);
        }
        end.U32(EndSignature);
        end.U16(0);
        end.U16(0);
        end.U16(end64 ? Max16 : records.size());
        end.U16(end64 ? Max16 : records.size());
        end.U32(end64 ? Max32 : directorySize);
        end.U32(end64 ? Max32 : directoryOffset);
        end.U16(0);
        put(end.data);

        out.flush();
        if (!out) Fail("cannot write " + ToUtf8(temporary) + " (is the disk full?)");
        out.close();

        std::error_code ec;
        // A virus scanner may hold a file it just saw change, which blocks replacing it for a moment.
        for (int attempt = 0; attempt < 20; ++attempt) {
            std::filesystem::rename(temporary, file, ec);
            if (ec != std::errc::permission_denied) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (ec) Fail("cannot replace " + ToUtf8(file) + ": " + ec.message());
        error.clear();
        return true;
    } catch (const std::exception& e) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = e.what();
        return false;
    }
}

} // namespace engine
