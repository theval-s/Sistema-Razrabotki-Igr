
#include "engine/RenderingSystem.hpp"

int main() {
    engine::RenderingSystem renderingSystem;
    renderingSystem.Initialize();
    
    renderingSystem.RenderFrame();
    return 0;
}
