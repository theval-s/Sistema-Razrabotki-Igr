#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <windows.h>
#include <spdlog/spdlog.h>

#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/rendering/RenderExtractionSubsystem.hpp>
#include <engine/rendering/RenderComponents.hpp>
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
using DirectX::SimpleMath::Vector3;

constexpr std::string_view CubeId = "e862521f-5ad3-4942-bbd6-aa8dbddae52e";

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

void AimLight(engine::Transform& transform, const Vector3& direction) {
    const float yaw = std::atan2(-direction.x, -direction.z);
    const float pitch = std::asin((std::clamp)(direction.y, -1.0f, 1.0f));
    transform.rotation = DirectX::SimpleMath::Quaternion::CreateFromYawPitchRoll(yaw, pitch, 0.0f);
}

} // namespace

/// Example-only animation; authored mode selects a motion pattern. No game state
/// is saved for these decorative lights.
class ExampleLightController final : public engine::Controller {
public:
    void LoadAuthored(const engine::StateValue& data) override { mode_ = data.String(); }
    void Update(double deltaSeconds) override {
        time_ += static_cast<float>(deltaSeconds);
        auto& scene = GetScene();
        auto& transform = scene.GetTransform(GetEntity());
        if (mode_ == "orange") {
            transform.position = {
                std::cos(time_ * 0.8f) * 4.0f,
                2.2f + std::sin(time_ * 1.7f) * 0.4f,
                std::sin(time_ * 0.8f) * 4.0f
            };
        } else if (mode_ == "blue") {
            transform.position = {
                std::cos(-time_ * 0.55f) * 3.0f, 1.4f, std::sin(-time_ * 0.55f) * 3.0f
            };
            if (auto* light = scene.TryGetComponent<engine::PointLight>(GetEntity()))
                light->intensity = 4.0f + std::sin(time_ * 2.2f) * 1.5f;
        } else if (mode_ == "green") {
            Vector3 direction{std::sin(time_ * 0.7f) * 0.8f, -1.0f, std::cos(time_ * 0.7f) * 0.8f};
            direction.Normalize();
            AimLight(transform, direction);
        } else if (mode_ == "magenta") {
            transform.position = {
                std::cos(time_ * 0.45f) * 5.0f, 4.5f, std::sin(time_ * 0.45f) * 5.0f
            };
            Vector3 direction = -transform.position;
            direction.Normalize();
            AimLight(transform, direction);
        }
    }
private:
    std::string mode_;
    float time_ = 0.0f;
};

engine::ControllerRegistration LightRegistration() {
    return {1,
        [](const engine::StateValue& data, std::string& error) {
            const auto mode = data.String();
            if (mode == "orange" || mode == "blue" || mode == "green" || mode == "magenta") return true;
            error = "unknown light motion mode";
            return false;
        },
        []() -> std::unique_ptr<engine::Controller> { return std::make_unique<ExampleLightController>(); }};
}

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

/// Input and camera control. Scene controllers animate lights; the renderer
/// extracts all scene meshes, materials and lights after controller updates.
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

        CaptureMouse(window_);
        return true;
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
            std::string error;
            if (!GetEngine().LoadGame("game/example.save.json", error)) spdlog::error("Load game: {}", error);
            else if (!error.empty()) spdlog::warn("Load game: {}", error);
        }
        if (GetAsyncKeyState('R') & 1) {
            const Matrix rotation = Matrix::CreateFromYawPitchRoll(world.camera.yaw, world.camera.pitch, 0);
            const Vector3 direction = Vector3::TransformNormal(Vector3::Forward, rotation);
            const auto hits = engine::RaycastAll(GetEngine().GetScene(), GetEngine().GetSceneAssets(),
                world.camera.position, direction, 100.0f);
            if (!hits.empty()) spdlog::info("Ray hit {} at {}", GetEngine().GetScene().GetName(hits[0].entity), hits[0].distance);
        }
        const float time = static_cast<float>(frame.simulationTimeSeconds);
        auto& scene = GetEngine().GetScene();
        if (const auto cube = scene.FindById(CubeId); cube != entt::null) {
            auto& transform = scene.GetTransform(cube);
            transform.rotation = DirectX::SimpleMath::Quaternion::CreateFromYawPitchRoll(time * 0.35f, 0, 0);
            transform.position.y = 3.8f + 0.16f * std::sin(time * 1.7f);
        }
    }

private:
    HWND window_ = nullptr;
    engine::RenderingSubsystem* rendering_ = nullptr;
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
        !engine.GetScene().RegisterController("example/hammer", MotionRegistration(true)) ||
        !engine.GetScene().RegisterController("example/light-motion", LightRegistration())) return 1;
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
