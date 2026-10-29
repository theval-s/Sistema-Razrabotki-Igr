#include <engine/core/FileSystem.hpp>

#include <fstream>
#include <random>
#include <system_error>
#include <utility>

#include <engine/core/PathUtils.hpp>

namespace engine {
namespace {

template <typename Buffer>
bool ReadWhole(const std::filesystem::path& file, Buffer& out, std::string& error, const std::uintmax_t maxSize) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
        error = "cannot read " + ToUtf8(file) + ": " + ec.message();
        return false;
    }
    if (size > maxSize) {
        error = "cannot read " + ToUtf8(file) + ": " + std::to_string(size) + " bytes is over the limit of " +
                std::to_string(maxSize);
        return false;
    }
    Buffer data(static_cast<std::size_t>(size), typename Buffer::value_type{});
    std::ifstream input(file, std::ios::binary);
    if (!input || !input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size))) {
        error = "cannot read " + ToUtf8(file);
        return false;
    }
    out = std::move(data);
    return true;
}

}  // namespace

bool ReadTextFile(const std::filesystem::path& file, std::string& text, std::string& error,
                  const std::uintmax_t maxSize) {
    std::string data;
    if (!ReadWhole(file, data, error, maxSize)) return false;
    if (data.starts_with("\xEF\xBB\xBF")) data.erase(0, 3);
    text = std::move(data);
    return true;
}

bool ReadBinaryFile(const std::filesystem::path& file, std::vector<std::uint8_t>& data, std::string& error,
                    const std::uintmax_t maxSize) {
    return ReadWhole(file, data, error, maxSize);
}

bool WriteFileAtomic(const std::filesystem::path& file, const std::string_view data, std::string& error) {
    thread_local std::mt19937_64 random{std::random_device{}()};
    auto temporary = file;
    temporary += ".tmp." + std::to_string(random());
    if (!CreateNewFile(temporary, data, error)) return false;
    std::error_code ec;
    std::filesystem::rename(temporary, file, ec);  // replaces an existing file
    if (ec) {
        error = "cannot replace " + ToUtf8(file) + ": " + ec.message();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    error.clear();
    return true;
}

bool CreateNewFile(const std::filesystem::path& file, const std::string_view data, std::string& error) {
    // might be possible to use std::ios::noreplace openmode if we had c++23
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) {
        error = ToUtf8(file) + " already exists";
        return false;
    }
    bool written = false;
    {
        std::ofstream output(file, std::ios::binary);
        written = output && output.write(data.data(), static_cast<std::streamsize>(data.size())) && output.flush();
    }
    if (!written) {
        error = "cannot write " + ToUtf8(file);
        std::filesystem::remove(file, ec);
        return false;
    }
    error.clear();
    return true;
}

}  // namespace engine
