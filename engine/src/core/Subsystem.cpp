#include <engine/core/Subsystem.hpp>

#include <cassert>

#include <engine/core/FrameContext.hpp>

namespace engine {

Subsystem::~Subsystem() = default;

bool Subsystem::Initialize() {
    return true;
}
void Subsystem::Shutdown() {}
void Subsystem::Update(const FrameContext&) {}
void Subsystem::Render(const FrameContext&) {}

Engine& Subsystem::GetEngine() const {
    assert(engine_ != nullptr && "Subsystem is not registered in an Engine");
    return *engine_;
}

}  // namespace engine
