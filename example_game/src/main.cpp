#include <engine/engine.hpp>

#include <iostream>

int main() {
    engine::Engine engine;
    std::cout << "SRI Engine " << engine::version() << '\n';
    return 0;
}
