#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/rendering/UglyUtils.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/engine.hpp>
#include <engine/window.hpp>

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
    engine::MaterialInstanceHandle materialInstance;
};

struct alignas(16) EnchantmentBufferData {
    float time;
    float padding[3]{};
};

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

engine::TextureHandle MakeSolidTexture(engine::RenderingResourceManager& resourceManager, uint32_t color) {
    const D3D11_SUBRESOURCE_DATA data{
        .pSysMem = &color,
        .SysMemPitch = sizeof(color),
        .SysMemSlicePitch = 0
    };
    const engine::TextureDesc description{
        .width = 1,
        .height = 1,
        .format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        .usage = engine::TextureUsage::ShaderResource
    };
    return resourceManager.CreateTexture(description, std::span<const D3D11_SUBRESOURCE_DATA>{&data, 1});
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
            .materialInstance = object.materialInstance
        });
    }
}

void UpdateLights(engine::RenderWorld& world, float time) {
    world.lights[0].position = Vector3{
        std::cos(time * 0.8f) * 4.0f,
        2.2f + std::sin(time * 1.7f) * 0.4f,
        std::sin(time * 0.8f) * 4.0f
    };
    world.lights[1].position = Vector3{
        std::cos(-time * 0.55f) * 3.0f,
        1.4f,
        std::sin(-time * 0.55f) * 3.0f
    };
    world.lights[1].intensity = 4.0f + std::sin(time * 2.2f) * 1.5f;

    Vector3 sweepingDirection{std::sin(time * 0.7f) * 0.8f, -1.0f, std::cos(time * 0.7f) * 0.8f};
    sweepingDirection.Normalize();
    world.lights[2].direction = sweepingDirection;

    world.lights[3].position = Vector3{
        std::cos(time * 0.45f) * 5.0f,
        4.5f,
        std::sin(time * 0.45f) * 5.0f
    };
    Vector3 trackingDirection = Vector3::Zero - world.lights[3].position;
    trackingDirection.Normalize();
    world.lights[3].direction = trackingDirection;
}

} // namespace

/// Host side of the example game: pumps Win32 messages, drives the FPS camera,
/// the animated props and the lights, and feeds RenderWorld. The models themselves
/// arrive through the AssetSystem in the background.
class ExampleGameLogic final : public engine::Subsystem {
public:
    explicit ExampleGameLogic(HWND window) : window_(window) {
    }

    [[nodiscard]] std::string_view GetName() const override {
        return "ExampleGameLogic";
    }

    [[nodiscard]] bool Initialize() override {
        rendering_ = GetEngine().GetSubsystem<engine::RenderingSubsystem>();
        if (rendering_ == nullptr) {
            return false;
        }
        assets_ = &GetEngine().GetAssets();
        auto& resources = rendering_->GetResourceManager();
        auto& world = rendering_->GetWorld();

        world.camera = {
            .fov = Pi / 3.0f,
            .aspectRatio = static_cast<float>(ScreenWidth) / static_cast<float>(ScreenHeight),
            .nearPlane = 0.1f,
            .farPlane = 100.0f,
            .yaw = 0.0f,
            .pitch = -0.15f,
            .position = Vector3{0.0f, 3.0f, 7.0f}
        };
        world.directionalLight = {
            .position = Vector3::Zero,
            .direction = Vector3{-0.45f, -1.0f, -0.3f},
            .color = Vector3{1.0f, 0.95f, 0.85f},
            .intensity = 1.25f,
            .range = 0.0f,
            .type = engine::LightInfo::Directional
        };
        constexpr float firstSpotOuterHalfAngle = Pi / 7.0f;
        constexpr float secondSpotOuterHalfAngle = Pi / 6.0f;
        const engine::MeshHandle firstSpotBoundingBox = engine::ugly_utils::FromRawMesh(
            resources, engine::ugly_utils::CreateSpotLightBoundingBox(10.0f, firstSpotOuterHalfAngle * 2.0f));
        const engine::MeshHandle secondSpotBoundingBox = engine::ugly_utils::FromRawMesh(
            resources, engine::ugly_utils::CreateSpotLightBoundingBox(8.0f, secondSpotOuterHalfAngle * 2.0f));
        world.lights = {
            {
                .position = Vector3{4.0f, 2.2f, 0.0f},
                .direction = Vector3::Zero,
                .color = Vector3{1.0f, 0.25f, 0.08f},
                .intensity = 5.0f,
                .range = 5.5f,
                .type = engine::LightInfo::Point
            },
            {
                .position = Vector3{-3.0f, 1.4f, 0.0f},
                .direction = Vector3::Zero,
                .color = Vector3{0.08f, 0.35f, 1.0f},
                .intensity = 4.0f,
                .range = 4.5f,
                .type = engine::LightInfo::Point
            },
            {
                .position = Vector3{0.0f, 6.0f, 0.0f},
                .direction = Vector3{0.0f, -1.0f, 0.0f},
                .color = Vector3{0.2f, 1.0f, 0.35f},
                .intensity = 7.0f,
                .range = 10.0f,
                .type = engine::LightInfo::Spot,
                .lightParameters = {
                    .spotLight = {
                        .innerAngle = std::cos(Pi / 12.0f),
                        .outerAngle = std::cos(firstSpotOuterHalfAngle),
                        .range = 10.0f,
                        .boundingBox = firstSpotBoundingBox
                    }
                }
            },
            {
                .position = Vector3{5.0f, 4.5f, 0.0f},
                .direction = Vector3{-1.0f, -1.0f, 0.0f},
                .color = Vector3{0.85f, 0.15f, 1.0f},
                .intensity = 6.0f,
                .range = 8.0f,
                .type = engine::LightInfo::Spot,
                .lightParameters = {
                    .spotLight = {
                        .innerAngle = std::cos(Pi / 10.0f),
                        .outerAngle = std::cos(secondSpotOuterHalfAngle),
                        .range = 8.0f,
                        .boundingBox = secondSpotBoundingBox
                    }
                }
            }
        };

        groundTexture_ = MakeCheckerTexture(resources);
        defaultMaterial_ = resources.CreateMaterial({});
        groundMaterial_ = resources.CreateMaterial({
            .shaderFile = L"game/GroundMaterial.hlsl",
            .hooks = engine::MaterialHook::GBufferVS | engine::MaterialHook::GBufferPS |
                engine::MaterialHook::ShadowVS
        });
        enchantedMaterial_ = resources.CreateMaterial({
            .shaderFile = L"game/EnchantedCube.hlsl",
            .hooks = engine::MaterialHook::GBufferVS | engine::MaterialHook::GBufferPS |
                engine::MaterialHook::ShadowVS
        });
        if (!defaultMaterial_ || !groundMaterial_ || !enchantedMaterial_) return false;
        enchantmentBuffer_ = resources.CreateConstantBuffer<EnchantmentBufferData>();
        lightMarkerMesh_ = engine::ugly_utils::FromRawMesh(
            resources, engine::ugly_utils::CreateSphere(0.15f));
        lightMarkerTextures_ = {
            MakeSolidTexture(resources, 0xff1440ffu),
            MakeSolidTexture(resources, 0xffff5914u),
            MakeSolidTexture(resources, 0xff59ff33u),
            MakeSolidTexture(resources, 0xffff26d9u)
        };
        objects_.push_back({
            .transform = Matrix::Identity,
            .mesh = MakeGroundMesh(resources),
            .materialInstance = MakeInstance(groundMaterial_, groundTexture_,
                {Vector3{0.08f, 0.08f, 0.08f}, 24.0f})
        });
        cubeIndex_ = objects_.size();
        objects_.push_back({
            .transform = Matrix::CreateTranslation(0.0f, 3.8f, 0.0f),
            .mesh = engine::ugly_utils::FromRawMesh(resources, engine::ugly_utils::CreateCube(1.6f, 1.6f, 1.6f)),
            .materialInstance = MakeInstance(enchantedMaterial_, {},
                {Vector3{0.75f, 0.48f, 0.95f}, 100.0f}, enchantmentBuffer_)
        });

        tableMesh_ = assets_->RequestMesh("models/Table", true);
        signMesh_ = assets_->RequestMesh("models/Sign", true);
        hammerMesh_ = assets_->RequestMesh("models/Hammer", true);
        editorCubeMesh_ = assets_->RequestMesh(engine::BuiltinAssets::CubeMesh);
        tableTexture_ = assets_->RequestTexture("models/Table");
        signTexture_ = assets_->RequestTexture("models/Sign");
        hammerTexture_ = assets_->RequestTexture("models/Hammer");

        CaptureMouse(window_);
        return true;
    }

    void Shutdown() override {
        auto& resources = rendering_->GetResourceManager();
        rendering_->GetWorld().renderItems.clear();
        for (const auto instance : instances_) resources.DestroyMaterialInstance(instance);
        if (enchantedMaterial_) resources.DestroyMaterial(enchantedMaterial_);
        if (groundMaterial_) resources.DestroyMaterial(groundMaterial_);
        if (defaultMaterial_) resources.DestroyMaterial(defaultMaterial_);
        if (enchantmentBuffer_) resources.DestroyBuffer(enchantmentBuffer_);
        instances_.clear();
    }

    void Update(const engine::FrameContext& frame) override {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                GetEngine().RequestExit();
                return;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!IsWindow(window_)) {
            GetEngine().RequestExit();
            return;
        }

        auto& world = rendering_->GetWorld();
        const auto& surface = GetEngine().GetSurface();
        if (surface.width > 0 && surface.height > 0) {
            world.camera.aspectRatio = static_cast<float>(surface.width) / static_cast<float>(surface.height);
        }
        UpdateCamera(world.camera, window_, static_cast<float>(frame.deltaSeconds));
        if (!sceneBuilt_) {
            TryBuildScene();
        }

        const float time = static_cast<float>(frame.simulationTimeSeconds);
        rendering_->GetContext().UpdateCBuffer(enchantmentBuffer_, EnchantmentBufferData{time});
        objects_[cubeIndex_].transform = Matrix::CreateRotationY(time * 0.35f) *
            Matrix::CreateTranslation(0.0f, 3.8f + 0.16f * std::sin(time * 1.7f), 0.0f);
        if (sceneBuilt_) {
            objects_[orbitingSign_].transform = Matrix::CreateScale(1.5f) *
                Matrix::CreateRotationY(-time * 1.2f) *
                Matrix::CreateTranslation(std::cos(time) * 5.0f, 1.25f,
                                          std::sin(time) * 5.0f);
            objects_[bobbingHammer_].transform = Matrix::CreateScale(1.4f) *
                Matrix::CreateRotationX(time * 1.8f) *
                Matrix::CreateRotationY(time * 0.9f) *
                Matrix::CreateTranslation(-4.5f, 1.5f + std::sin(time * 2.0f), 3.0f);
            objects_[spinningHammer_].transform = Matrix::CreateScale(1.2f) *
                Matrix::CreateRotationZ(time * 2.5f) *
                Matrix::CreateTranslation(4.5f, 1.2f, 2.5f + std::sin(time) * 1.5f);
        }

        UpdateLights(world, time);
        if (sceneBuilt_) {
            for (size_t i = 0; i < lightMarkerTextures_.size(); ++i) {
                objects_[firstLightMarker_ + i].transform =
                    Matrix::CreateTranslation(world.lights[i].position);
            }
        }
        FillRenderWorld(world, objects_);
    }

private:
    engine::MaterialInstanceHandle MakeInstance(engine::MaterialHandle material, engine::TextureHandle texture,
                                                engine::MaterialPBRParameters pbr,
                                                engine::BufferHandle customBuffer = {}) {
        auto& resources = rendering_->GetResourceManager();
        const auto instance = resources.CreateMaterialInstance(material);
        engine::MaterialParameters params{};
        params.albedoTexture = texture;
        params.parameters = pbr;
        params.customBuffer = customBuffer;
        resources.SetMaterialInstanceParameters(instance, params);
        instances_.push_back(instance);
        return instance;
    }

    void TryBuildScene() {
        const auto tableMesh = assets_->Peek(tableMesh_);
        const auto signMesh = assets_->Peek(signMesh_);
        const auto hammerMesh = assets_->Peek(hammerMesh_);
        const auto editorCubeMesh = assets_->Peek(editorCubeMesh_);
        const auto tableTexture = assets_->Peek(tableTexture_);
        const auto signTexture = assets_->Peek(signTexture_);
        const auto hammerTexture = assets_->Peek(hammerTexture_);
        if (!tableMesh || !signMesh || !hammerMesh || !editorCubeMesh ||
            !tableTexture || !signTexture || !hammerTexture) {
            return;
        }

        const engine::MaterialPBRParameters objectMaterial{
            Vector3{0.25f, 0.25f, 0.25f}, 0.0f
        };
        SceneObject table{
            .transform = Matrix::CreateScale(3.5f) * Matrix::CreateTranslation(-2.5f, 0.55f, 0.0f),
            .mesh = tableMesh->mesh,
            .materialInstance = MakeInstance(defaultMaterial_, tableTexture->texture,
                {objectMaterial.specularColor, 64.0f})
        };
        SceneObject sign{
            .transform = Matrix::CreateScale(3.0f) * Matrix::CreateRotationY(-0.35f) *
            Matrix::CreateTranslation(2.5f, 1.5f, -1.0f),
            .mesh = signMesh->mesh,
            .materialInstance = MakeInstance(defaultMaterial_, signTexture->texture,
                {objectMaterial.specularColor, 32.0f})
        };
        SceneObject hammer{
            .transform = Matrix::CreateScale(2.0f) * Matrix::CreateRotationZ(-0.65f) *
            Matrix::CreateTranslation(0.0f, 1.0f, 1.5f),
            .mesh = hammerMesh->mesh,
            .materialInstance = MakeInstance(defaultMaterial_, hammerTexture->texture,
                {objectMaterial.specularColor, 96.0f})
        };
        objects_.push_back(table);
        objects_.push_back(sign);
        objects_.push_back(hammer);
        // Same mesh, dimensions and default white material as the editor's Add Cube action.
        objects_.push_back({
            .transform = Matrix::CreateTranslation(-5.0f, 0.5f, -4.0f),
            .mesh = editorCubeMesh->mesh,
            .materialInstance = MakeInstance(defaultMaterial_, {}, {})
        });

        orbitingSign_ = objects_.size();
        objects_.push_back(sign);
        bobbingHammer_ = objects_.size();
        objects_.push_back(hammer);
        spinningHammer_ = objects_.size();
        objects_.push_back(hammer);

        firstLightMarker_ = objects_.size();
        for (const engine::TextureHandle texture : lightMarkerTextures_) {
            objects_.push_back({
                .transform = Matrix::Identity,
                .mesh = lightMarkerMesh_,
                .materialInstance = MakeInstance(defaultMaterial_, texture,
                    {Vector3{0.4f, 0.4f, 0.4f}, 96.0f})
            });
        }
        sceneBuilt_ = true;
    }

    HWND window_ = nullptr;
    engine::RenderingSubsystem* rendering_ = nullptr;
    engine::AssetSystem* assets_ = nullptr;

    engine::TextureHandle groundTexture_;
    engine::MaterialHandle defaultMaterial_;
    engine::MaterialHandle groundMaterial_;
    engine::MaterialHandle enchantedMaterial_;
    engine::BufferHandle enchantmentBuffer_;
    size_t cubeIndex_ = 0;
    std::vector<engine::MaterialInstanceHandle> instances_;
    engine::MeshHandle lightMarkerMesh_;
    std::vector<engine::TextureHandle> lightMarkerTextures_;

    engine::AssetHandle<engine::MeshAsset> tableMesh_;
    engine::AssetHandle<engine::MeshAsset> signMesh_;
    engine::AssetHandle<engine::MeshAsset> hammerMesh_;
    engine::AssetHandle<engine::MeshAsset> editorCubeMesh_;
    engine::AssetHandle<engine::TextureAsset> tableTexture_;
    engine::AssetHandle<engine::TextureAsset> signTexture_;
    engine::AssetHandle<engine::TextureAsset> hammerTexture_;

    std::vector<SceneObject> objects_;
    bool sceneBuilt_ = false;
    size_t orbitingSign_ = 0;
    size_t bobbingHammer_ = 0;
    size_t spinningHammer_ = 0;
    size_t firstLightMarker_ = 0;
};


int main(int /*argumentCount*/, char** /*arguments*/) {
    static engine::Engine* enginePtr;

    engine::WindowResizedCallback* windowCallback = [](int newWidth, int newHeight){
        if (enginePtr) {
            enginePtr->ResizeSurface(newWidth, newHeight);
        }
    };

    HWND window = engine::makeWindow(ScreenWidth, ScreenHeight, windowCallback);


    engine::EngineConfig config;
    config.applicationName = "ExampleGame";
    config.surface = engine::NativeSurface{window, ScreenWidth, ScreenHeight};

    engine::Engine engine{config};
    engine.AddSubsystem<engine::RenderingSubsystem>();
    engine.AddSubsystem<ExampleGameLogic>(window);
    enginePtr = &engine;
    const bool ran = engine.Run();

    ReleaseCapture();
    while (ShowCursor(TRUE) < 0) {
    }
    return ran ? 0 : 1;
}
