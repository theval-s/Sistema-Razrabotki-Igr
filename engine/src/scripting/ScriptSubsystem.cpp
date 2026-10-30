#include <engine/scripting/ScriptSubsystem.hpp>

#include <string>
#include <exception>
#include <utility>

#include <engine/core/FrameContext.hpp>

namespace engine {

std::string_view ScriptSubsystem::GetName() const {
    return "ScriptSubsystem";
}

bool ScriptSubsystem::Initialize() {
    ready_ = true;
    return true;
}

void ScriptSubsystem::Shutdown() {
    EndPlay();
    ready_ = false;
}

bool ScriptSubsystem::BeginPlay(const std::filesystem::path& root, std::string& error) {
    if (!ready_) {
        error = "ScriptSubsystem is not initialized";
        return false;
    }
    if (playing_) {
        error = "A scripting Play session is already active";
        return false;
    }
    std::filesystem::path absolute;
    try {
        absolute = std::filesystem::weakly_canonical(std::filesystem::absolute(root));
        if (root.empty() || !std::filesystem::is_directory(absolute)) {
            error = "The scripting root must be an existing directory";
            return false;
        }
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    // Also discard any state created through the compatibility runtime accessor.
    runtime_.Shutdown();
    if (!runtime_.Initialize(absolute, error)) return false;
    root_ = std::move(absolute);
    playing_ = true;
    return true;
}

void ScriptSubsystem::EndPlay() noexcept {
    playing_ = false;
    runtime_.Shutdown();
    root_.clear();
}

void ScriptSubsystem::Update(const FrameContext& frame) {
    if (!playing_ || frame.paused || frame.deltaSeconds <= 0.0) return;
    std::string error;
    (void)runtime_.UpdateInstances(frame.deltaSeconds, error);  // Runtime logs instance diagnostics.
}

}  // namespace engine
