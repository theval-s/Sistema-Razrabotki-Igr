#include <engine/core/Log.hpp>

#include <exception>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace engine::log {

namespace {
// spdlog's filename type is configured when its shared library is built. Do not
// change SPDLOG_WCHAR_FILENAMES here: that would change the shared-library ABI.
class NativeRotatingSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    NativeRotatingSink(std::filesystem::path path, std::size_t maxBytes, std::size_t files)
        : path_(std::move(path)), maxBytes_(maxBytes), files_(files) {
        if (maxBytes_ == 0) throw spdlog::spdlog_ex("maximum log size must be nonzero");
        open();
        currentSize_ = static_cast<std::size_t>(std::filesystem::file_size(path_));
    }

private:
    void open() {
        stream_.open(path_, std::ios::binary | std::ios::app);
        if (!stream_) throw spdlog::spdlog_ex("cannot open log file");
    }

    std::filesystem::path rotated(std::size_t index) const {
        return path_.parent_path() / (path_.stem().native() +
            std::filesystem::path("." + std::to_string(index)).native() + path_.extension().native());
    }

    void rotate() {
        stream_.close();
        if (files_ == 0) {
            stream_.open(path_, std::ios::binary | std::ios::trunc);
            if (!stream_) throw spdlog::spdlog_ex("cannot truncate log file");
        } else {
            for (std::size_t i = files_; i > 0; --i) {
                const auto source = i == 1 ? path_ : rotated(i - 1);
                const auto target = rotated(i);
                if (std::filesystem::exists(source)) {
                    std::filesystem::remove(target);
                    std::filesystem::rename(source, target);
                }
            }
            open();
        }
        currentSize_ = 0;
    }

    void sink_it_(const spdlog::details::log_msg& message) override {
        if (failed_) return;
        try {
            spdlog::memory_buf_t formatted;
            formatter_->format(message, formatted);
            if (currentSize_ > 0 && (currentSize_ >= maxBytes_ || formatted.size() > maxBytes_ - currentSize_)) rotate();
            stream_.write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
            if (!stream_) throw spdlog::spdlog_ex("cannot write log file");
            currentSize_ += formatted.size();
        } catch (const std::exception& e) {
            fail(e.what());
        } catch (...) {
            fail("unknown error");
        }
    }

    void flush_() override {
        if (failed_) return;
        try {
            stream_.flush();
            if (!stream_) throw spdlog::spdlog_ex("cannot flush log file");
        } catch (const std::exception& e) {
            fail(e.what());
        } catch (...) {
            fail("unknown error");
        }
    }

    void fail(const char* reason) noexcept {
        // Never log through spdlog here: this sink is called under the dist sink's lock.
        // Disable it after the first failure, preserving delivery to downstream sinks.
        failed_ = true;
        if (stream_.is_open()) stream_.close();
        std::fprintf(stderr, "File logging disabled: %s\n", reason);
        std::fflush(stderr);
    }

    std::filesystem::path path_;
    std::size_t maxBytes_;
    std::size_t files_;
    std::size_t currentSize_ = 0;
    bool failed_ = false;
    std::ofstream stream_;
};
}  // namespace

bool AddFileSink(const std::filesystem::path& file, std::size_t maxBytes, std::size_t files) {
    try {
        if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path());
        Sinks()->add_sink(std::make_shared<NativeRotatingSink>(file, maxBytes, files));
        spdlog::flush_on(spdlog::level::warn);
        return true;
    }
    catch (const std::exception&) { return false; }
}

const std::shared_ptr<spdlog::sinks::dist_sink_mt>& Sinks() {
    static const std::shared_ptr<spdlog::sinks::dist_sink_mt> sinks = [] {
        auto dist = std::make_shared<spdlog::sinks::dist_sink_mt>();
        dist->add_sink(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("", dist));
        return dist;
    }();
    return sinks;
}

spdlog::logger& Channel(const std::string_view name) {
    // Kept here as well as in spdlog's registry, so spdlog::drop_all() can't free a logger
    // whose reference an accessor holds.
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<spdlog::logger>, std::less<>> channels;

    std::lock_guard lock(mutex);
    if (const auto it = channels.find(name); it != channels.end()) return *it->second;
    auto logger = std::make_shared<spdlog::logger>(std::string(name), Sinks());
    // The global level, flush level and pattern, and registration by name.
    spdlog::initialize_logger(logger);
    return *channels.emplace(std::string(name), std::move(logger)).first->second;
}

}  // namespace engine::log
