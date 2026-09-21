#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include <windows.h>

#include "engine/RenderingSystem.hpp"
#include "engine/UglyUtils.hpp"

namespace {

constexpr uint32_t ScreenWidth = 1280;
constexpr uint32_t ScreenHeight = 720;
constexpr float Pi = 3.14159265358979323846f;

using DirectX::SimpleMath::Matrix;
using DirectX::SimpleMath::Vector2;
using DirectX::SimpleMath::Vector3;
using DirectX::SimpleMath::Vector4;

struct SceneObject {
    Matrix transform;
    engine::MeshHandle mesh;
    engine::TextureHandle texture;
    engine::MaterialParameters material;
};

engine::MeshHandle MakeObjMesh(engine::RenderingResourceManager& resourceManager,
                               const engine::ugly_utils::ObjModel& model) {
    engine::ugly_utils::RawMesh rawMesh;
    rawMesh.vertexes.reserve(model.vertices.size());
    for (const auto& source : model.vertices) {
        rawMesh.vertexes.push_back({
            Vector4{source.position.x, source.position.y, source.position.z, 1.0f},
            source.normal,
            source.uv
        });
    }
    rawMesh.indicies = model.indices;
    return engine::ugly_utils::FromRawMesh(resourceManager, rawMesh);
}

engine::MeshHandle MakeGroundMesh(engine::RenderingResourceManager& resourceManager) {
    constexpr float extent = 15.0f;
    constexpr float repetitions = 12.0f;
    const Vector3 normal = Vector3::Up;
    engine::ugly_utils::RawMesh mesh{
        .vertexes = {
            {Vector4{-extent, 0.0f, extent, 1.0f}, normal, Vector2{0.0f, 0.0f}},
            {Vector4{extent, 0.0f, extent, 1.0f}, normal, Vector2{repetitions, 0.0f}},
            {Vector4{extent, 0.0f, -extent, 1.0f}, normal, Vector2{repetitions, repetitions}},
            {Vector4{-extent, 0.0f, -extent, 1.0f}, normal, Vector2{0.0f, repetitions}}
        },
        .indicies = {0, 2, 1, 2, 0, 3}
    };
    return engine::ugly_utils::FromRawMesh(resourceManager, mesh);
}

engine::TextureHandle MakeCheckerTexture(engine::RenderingResourceManager& resourceManager) {
    constexpr uint32_t size = 128;
    constexpr uint32_t cellSize = 16;
    std::vector<uint32_t> pixels(size * size);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const bool bright = ((x / cellSize) + (y / cellSize)) % 2 == 0;
            pixels[y * size + x] = bright ? 0xff70766fu : 0xff343a36u;
        }
    }

    const D3D11_SUBRESOURCE_DATA data{
        .pSysMem = pixels.data(),
        .SysMemPitch = size * static_cast<uint32_t>(sizeof(uint32_t)),
        .SysMemSlicePitch = 0
    };
    const engine::TextureDesc description{
        .width = size,
        .height = size,
        .format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        .usage = engine::TextureUsage::ShaderResource
    };
    return resourceManager.CreateTexture(description, std::span<const D3D11_SUBRESOURCE_DATA>{&data, 1});
}

SceneObject LoadObject(engine::RenderingSystem& renderer, const wchar_t* folder, const Matrix& transform,
                       float shininess) {
    engine::ugly_utils::RenderingState state{
        renderer.renderingContext.device_.Get(),
        renderer.renderingContext.deviceContext_.Get()
    };
    auto model = engine::ugly_utils::LoadObjModel(state, folder, true);
    const engine::MeshHandle mesh = MakeObjMesh(renderer.resourceManager, model);
    const engine::TextureHandle texture = renderer.resourceManager.RegisterTexture(
        engine::Texture{std::move(model.textureSRV)});
    return SceneObject{
        .transform = transform,
        .mesh = mesh,
        .texture = texture,
        .material = {
            .specularColor = Vector3{0.25f, 0.25f, 0.25f},
            .shininess = shininess,
            .specialType = 0
        }
    };
}

bool IsKeyDown(int key) {
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

void CaptureMouse(HWND window) {
    SetCapture(window);
    while (ShowCursor(FALSE) >= 0) {
    }
    RECT client{};
    GetClientRect(window, &client);
    POINT center{(client.right - client.left) / 2, (client.bottom - client.top) / 2};
    ClientToScreen(window, &center);
    SetCursorPos(center.x, center.y);
}

void UpdateCamera(engine::FPSCamera& camera, HWND window, float deltaTime) {
    const bool mouseCaptured = GetCapture() == window;
    if (!mouseCaptured && (GetAsyncKeyState(VK_LBUTTON) & 1) != 0 && GetForegroundWindow() == window) {
        CaptureMouse(window);
    }

    if (GetCapture() == window && GetForegroundWindow() == window) {
        RECT client{};
        GetClientRect(window, &client);
        POINT center{(client.right - client.left) / 2, (client.bottom - client.top) / 2};
        ClientToScreen(window, &center);
        POINT cursor{};
        GetCursorPos(&cursor);
        camera.yaw -= static_cast<float>(cursor.x - center.x) * 0.0035f;
        camera.pitch -= static_cast<float>(cursor.y - center.y) * 0.0035f;
        camera.pitch = (std::clamp)(camera.pitch, -Pi * 0.49f, Pi * 0.49f);
        SetCursorPos(center.x, center.y);
    }

    const Matrix rotation = Matrix::CreateFromYawPitchRoll(camera.yaw, camera.pitch, 0.0f);
    const Vector3 forward = Vector3::TransformNormal(Vector3::Forward, rotation);
    const Vector3 right = Vector3::TransformNormal(Vector3::Right, rotation);
    Vector3 movement{};
    if (IsKeyDown('W')) movement += forward;
    if (IsKeyDown('S')) movement -= forward;
    if (IsKeyDown('D')) movement += right;
    if (IsKeyDown('A')) movement -= right;
    if (IsKeyDown('E')) movement += Vector3::Up;
    if (IsKeyDown('Q')) movement -= Vector3::Up;

    if (movement.LengthSquared() > 0.0f) {
        movement.Normalize();
        const float speed = IsKeyDown(VK_SHIFT) ? 12.0f : 5.0f;
        camera.position += movement * speed * deltaTime;
    }
}

void FillRenderWorld(engine::RenderWorld& world, const std::vector<SceneObject>& objects) {
    world.renderItems.clear();
    world.renderItems.reserve(objects.size());
    for (const SceneObject& object : objects) {
        world.renderItems.push_back({
            .worldMatrix = object.transform,
            .mesh = object.mesh,
            .material = object.material,
            .texture = object.texture
        });
    }
}

} // namespace

int main(int argumentCount, char**) {
    engine::RenderingSystem renderer;
    renderer.Initialize(ScreenWidth, ScreenHeight);

    renderer.world.camera = {
        .fov = Pi / 3.0f,
        .aspectRatio = static_cast<float>(ScreenWidth) / static_cast<float>(ScreenHeight),
        .nearPlane = 0.1f,
        .farPlane = 100.0f,
        .yaw = 0.0f,
        .pitch = -0.15f,
        .position = Vector3{0.0f, 3.0f, 7.0f}
    };
    renderer.world.directionalLight = {
        .position = Vector3::Zero,
        .direction = Vector3{-0.45f, -1.0f, -0.3f},
        .color = Vector3{1.0f, 0.95f, 0.85f},
        .intensity = 1.25f,
        .range = 0.0f,
        .type = engine::LightInfo::Directional
    };

    const engine::TextureHandle groundTexture = MakeCheckerTexture(renderer.resourceManager);
    std::vector<SceneObject> objects;
    objects.push_back({
        .transform = Matrix::Identity,
        .mesh = MakeGroundMesh(renderer.resourceManager),
        .texture = groundTexture,
        .material = {Vector3{0.08f, 0.08f, 0.08f}, 24.0f, 0}
    });
    objects.push_back(LoadObject(renderer, L"models/Table", Matrix::CreateScale(3.5f) *
        Matrix::CreateTranslation(-2.5f, 0.55f, 0.0f), 64.0f));
    objects.push_back(LoadObject(renderer, L"models/Sign", Matrix::CreateScale(3.0f) *
        Matrix::CreateRotationY(-0.35f) * Matrix::CreateTranslation(2.5f, 1.5f, -1.0f), 32.0f));
    objects.push_back(LoadObject(renderer, L"models/Hammer", Matrix::CreateScale(2.0f) *
        Matrix::CreateRotationZ(-0.65f) * Matrix::CreateTranslation(0.0f, 1.0f, 1.5f), 96.0f));

    DXGI_SWAP_CHAIN_DESC swapChainDescription{};
    renderer.renderingContext.swapChain_->GetDesc(&swapChainDescription);
    const HWND window = swapChainDescription.OutputWindow;
    CaptureMouse(window);

    auto previousTime = std::chrono::steady_clock::now();
    bool running = true;
    while (running && IsWindow(window)) {
        MSG message{};
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = false;
            }
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        if (!running) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        const float deltaTime = (std::min)(std::chrono::duration<float>(now - previousTime).count(), 0.05f);
        previousTime = now;
        UpdateCamera(renderer.world.camera, window, deltaTime);
        FillRenderWorld(renderer.world, objects);
        renderer.RenderFrame();
    }

    ReleaseCapture();
    while (ShowCursor(TRUE) < 0) {
    }
    return 0;
}
