#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>
#include <spdlog/spdlog.h>

#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/rendering/RenderExtractionSubsystem.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/rendering/UglyUtils.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/engine.hpp>
#include <engine/scene/Controller.hpp>
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

class ExampleMotionController final : public engine::Controller {
public:
    explicit ExampleMotionController(bool hammer) : hammer_(hammer) {}
    void LoadAuthored(const engine::StateValue& data) override { speed_ = static_cast<float>(data.Number()); }
    void SaveAuthored(engine::StateValue& data) const override { data = static_cast<double>(speed_); }
    void LoadGameState(const engine::StateValue& data) override { time_ = static_cast<float>(data.At("time").Number()); }
    void SaveGameState(engine::StateValue& data) const override { data["time"] = static_cast<double>(time_); }
    void OnCreate() override { startPosition_ = GetScene().GetTransform(GetEntity()).position; }
    void Update(double deltaSeconds) override {
        time_ += static_cast<float>(deltaSeconds);
        auto& transform = GetScene().GetTransform(GetEntity());
        if (hammer_) {
            transform.position.y = startPosition_.y + std::sin(time_ * 2.0f) * 0.25f;
            transform.rotation = DirectX::SimpleMath::Quaternion::CreateFromYawPitchRoll(0, 0, -0.65f + time_ * speed_);
        } else {
            transform.rotation = DirectX::SimpleMath::Quaternion::CreateFromYawPitchRoll(-0.35f - time_ * speed_, 0, 0);
        }
    }
private:
    bool hammer_;
    float speed_ = 0;
    float time_ = 0;
    Vector3 startPosition_{};
};

engine::ControllerRegistration MotionRegistration(bool hammer) {
    return {1,
        [](const engine::StateValue& data, std::string& error) {
            if (std::isfinite(data.Number()) && data.Number() > 0) return true;
            error = "motion speed must be a positive number"; return false;
        },
        [hammer]() -> std::unique_ptr<engine::Controller> {
            return std::make_unique<ExampleMotionController>(hammer);
        }};
}

/// Host-owned procedural effects, input and animated point/spot lights. File-backed
/// meshes and materials come from demo.scene.json through RenderExtractionSubsystem.
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

        enchantmentBuffer_ = resources.CreateConstantBuffer<EnchantmentBufferData>();
        lightMarkerMesh_ = engine::ugly_utils::FromRawMesh(
            resources, engine::ugly_utils::CreateSphere(0.15f));
        lightMarkerTextures_ = {
            MakeSolidTexture(resources, 0xff1440ffu),
            MakeSolidTexture(resources, 0xffff5914u),
            MakeSolidTexture(resources, 0xff59ff33u),
            MakeSolidTexture(resources, 0xffff26d9u)
        };
        cubeIndex_ = objects_.size();
        objects_.push_back({
            .transform = Matrix::CreateTranslation(0.0f, 3.8f, 0.0f),
            .mesh = engine::ugly_utils::FromRawMesh(resources, engine::ugly_utils::CreateCube(1.6f, 1.6f, 1.6f)),
            .materialInstance = {}
        });

        firstLightMarker_ = objects_.size();
        for (size_t i = 0; i < lightMarkerTextures_.size(); ++i) {
            objects_.push_back({
                .transform = Matrix::Identity,
                .mesh = lightMarkerMesh_,
                .materialInstance = {}
            });
        }

        CaptureMouse(window_);
        return true;
    }

    void Shutdown() override {
        auto& resources = rendering_->GetResourceManager();
        rendering_->GetWorld().renderItems.clear();
        ReleaseSceneInstances();
        if (enchantmentBuffer_) resources.DestroyBuffer(enchantmentBuffer_);
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
        if (GetAsyncKeyState(VK_F5) & 1) {
            std::string error;
            if (!GetEngine().SaveGame("game/example.save.json", error)) spdlog::error("Save game: {}", error);
            else spdlog::info("Saved game/example.save.json");
        }
        if (GetAsyncKeyState(VK_F9) & 1) {
            ReleaseSceneInstances();
            std::string error;
            if (!GetEngine().LoadGame("game/example.save.json", error)) spdlog::error("Load game: {}", error);
            else if (!error.empty()) spdlog::warn("Load game: {}", error);
            RequestSceneMaterials();
        }
        // LoadScene unloads the scene asset scope. Request procedural materials
        // only after that initial load (and again after a save-game reload).
        if (!sceneMaterialsRequested_) RequestSceneMaterials();
        if (GetAsyncKeyState('R') & 1) {
            const Matrix rotation = Matrix::CreateFromYawPitchRoll(world.camera.yaw, world.camera.pitch, 0);
            const Vector3 direction = Vector3::TransformNormal(Vector3::Forward, rotation);
            const auto hits = engine::RaycastAll(GetEngine().GetScene(), GetEngine().GetSceneAssets(),
                world.camera.position, direction, 100.0f);
            if (!hits.empty()) spdlog::info("Ray hit {} at {}", GetEngine().GetScene().GetName(hits[0].entity), hits[0].distance);
        }
        const float time = static_cast<float>(frame.simulationTimeSeconds);
        if (!objects_[cubeIndex_].materialInstance)
            objects_[cubeIndex_].materialInstance = MakeInstance(enchantedInstance_, {}, enchantmentBuffer_);
        for (size_t i = 0; i < lightMarkerTextures_.size(); ++i)
            if (!objects_[firstLightMarker_ + i].materialInstance)
                objects_[firstLightMarker_ + i].materialInstance = MakeInstance(markerInstance_, lightMarkerTextures_[i]);
        rendering_->GetContext().UpdateCBuffer(enchantmentBuffer_, EnchantmentBufferData{time});
        objects_[cubeIndex_].transform = Matrix::CreateRotationY(time * 0.35f) *
            Matrix::CreateTranslation(0.0f, 3.8f + 0.16f * std::sin(time * 1.7f), 0.0f);

        UpdateLights(world, time);
        for (size_t i = 0; i < lightMarkerTextures_.size(); ++i) {
            objects_[firstLightMarker_ + i].transform =
                Matrix::CreateTranslation(world.lights[i].position);
        }
        FillRenderWorld(world, objects_);
    }

private:
    void RequestSceneMaterials() {
        auto& assets = GetEngine().GetSceneAssets();
        enchantedInstance_ = assets.RequestMaterialInstance(engine::AssetReference{"b9e1fbd1-dfa0-416b-857e-8823b736da81",
            "game/materials/enchanted.material-instance.json"});
        markerInstance_ = assets.RequestMaterialInstance(engine::AssetReference{"70da8a2f-d0b1-40fd-bd45-7ad5e610e855",
            "game/materials/glossy.material-instance.json"});
        sceneMaterialsRequested_ = true;
    }

    void ReleaseSceneInstances() {
        rendering_->GetWorld().renderItems.clear();
        auto& resources = rendering_->GetResourceManager();
        for (const auto instance : instances_) resources.DestroyMaterialInstance(instance);
        instances_.clear();
        for (auto& object : objects_) object.materialInstance = {};
    }

    engine::MaterialInstanceHandle MakeInstance(engine::AssetHandle<engine::MaterialInstanceAsset> asset,
                                                 engine::TextureHandle texture, engine::BufferHandle customBuffer = {}) {
        const auto ready = GetEngine().GetSceneAssets().Peek(asset);
        if (!ready) return {};
        auto& resources = rendering_->GetResourceManager();
        const auto* base = resources.TryGetMaterialInstance(ready->instance);
        if (!base) return {};
        const auto instance = resources.CreateMaterialInstance(base->material);
        if (!instance) return {};
        engine::MaterialParameters params = base->parameters;
        if (texture) params.albedoTexture = texture;
        params.customBuffer = customBuffer;
        if (!resources.SetMaterialInstanceParameters(instance, params)) {
            resources.DestroyMaterialInstance(instance);
            return {};
        }
        instances_.push_back(instance);
        return instance;
    }

    HWND window_ = nullptr;
    engine::RenderingSubsystem* rendering_ = nullptr;

    engine::AssetHandle<engine::MaterialInstanceAsset> enchantedInstance_;
    engine::AssetHandle<engine::MaterialInstanceAsset> markerInstance_;
    bool sceneMaterialsRequested_ = false;
    engine::BufferHandle enchantmentBuffer_;
    size_t cubeIndex_ = 0;
    std::vector<engine::MaterialInstanceHandle> instances_;
    engine::MeshHandle lightMarkerMesh_;
    std::vector<engine::TextureHandle> lightMarkerTextures_;

    std::vector<SceneObject> objects_;
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
    engine.AddSubsystem<engine::RenderExtractionSubsystem>();
    if (!engine.GetScene().RegisterController("example/sign", MotionRegistration(false)) ||
        !engine.GetScene().RegisterController("example/hammer", MotionRegistration(true))) return 1;
    enginePtr = &engine;
    if (!engine.Initialize()) return 1;
    std::string error;
    if (!engine.LoadScene(engine::AssetReference{"2a9a7998-f44d-44d8-8fc3-57e78da18796",
                                                 "game/scenes/demo.scene.json"}, error)) {
        spdlog::error("ExampleGame: cannot load scene: {}", error);
        return 1;
    }
    spdlog::info("ExampleGame: {} animated entities", engine.GetScene().FindWithTag("animated").size());
    const bool ran = engine.Run();

    ReleaseCapture();
    while (ShowCursor(TRUE) < 0) {
    }
    return ran ? 0 : 1;
}
