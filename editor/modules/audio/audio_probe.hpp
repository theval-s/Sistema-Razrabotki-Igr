#pragma once
#include <engine/audio/AudioSubsystem.hpp>
#include <atomic>
#include <exception>
#include <memory>
#include <thread>

namespace audio_editor {
// A lightweight polling key also detects replacements which preserve catalog identity/path.
struct FileStamp {
    std::uintmax_t bytes = 0;
    std::filesystem::file_time_type modified{};
    bool readable = false;
    bool operator==(const FileStamp&) const = default;
    static FileStamp read(const std::filesystem::path& path) {
        std::error_code error;
        const auto bytes = std::filesystem::file_size(path, error);
        if (error) return {};
        const auto modified = std::filesystem::last_write_time(path, error);
        if (error) return {};
        return {bytes, modified, true};
    }
};

// Workers access only a copied path and result storage, never Engine or Qt objects.
// Join at destruction so module unloading cannot leave a worker executing unloaded code.
struct Probe {
    std::atomic<bool> ready{false};
    engine::AudioClipInfo info;
    std::string error;
    std::thread worker;
    ~Probe() { if (worker.joinable()) worker.join(); }
    static std::shared_ptr<Probe> start(std::filesystem::path path) {
        auto result = std::make_shared<Probe>();
        result->worker = std::thread([storage = result.get(), path = std::move(path)] {
            try { (void)engine::InspectAudioClip(path, storage->info, storage->error); }
            catch (const std::exception& e) { storage->error = e.what(); }
            catch (...) { storage->error = "Audio inspection failed"; }
            storage->ready.store(true, std::memory_order_release);
        });
        return result;
    }
    bool complete() const { return ready.load(std::memory_order_acquire); }
};
}
