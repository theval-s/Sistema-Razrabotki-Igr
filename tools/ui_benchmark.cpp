#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/rendering/RenderingContext.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>
#include <engine/ui/UiSubsystem.hpp>
#include "ui/UiRuntimeAccess.hpp"

#include <Windows.h>
#include <Psapi.h>
#include <winternl.h>
#include <intrin.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using namespace engine;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
constexpr int warmup = 60, samples = 300;

void Require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
void Memory(const char* stage) {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    Require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters)) != FALSE, "GetProcessMemoryInfo failed");
    std::cout << "memory," << stage << ",private_bytes," << counters.PrivateUsage
              << ",working_set_bytes," << counters.WorkingSetSize << '\n';
}
void Machine(RenderingContext& graphics) {
    char name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD length = sizeof(name);
    GetComputerNameA(name, &length);
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    char brand[49]{};
    int registers[4]{};
    __cpuid(registers, static_cast<int>(0x80000000u));
    if (static_cast<unsigned>(registers[0]) >= 0x80000004u) {
        for (unsigned i = 0; i < 3; ++i) {
            __cpuid(registers, static_cast<int>(0x80000002u + i));
            std::memcpy(brand + i * 16, registers, 16);
        }
    }
    using VersionFunction = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const auto versionFunction = reinterpret_cast<VersionFunction>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    Require(versionFunction && versionFunction(&version) == 0, "RtlGetVersion failed");
    ComPtr<IDXGIDevice> device;
    ComPtr<IDXGIAdapter> adapter;
    Require(SUCCEEDED(graphics.GetDevice()->QueryInterface(IID_PPV_ARGS(device.GetAddressOf()))), "DXGI device");
    Require(SUCCEEDED(device->GetAdapter(adapter.GetAddressOf())), "DXGI adapter");
    DXGI_ADAPTER_DESC desc{};
    Require(SUCCEEDED(adapter->GetDesc(&desc)), "DXGI adapter description");
    std::cout << "machine," << name << ",Windows," << version.dwMajorVersion << '.' << version.dwMinorVersion
              << '.' << version.dwBuildNumber << ",CPU," << brand << ",logical_processors,"
              << info.dwNumberOfProcessors << ",MSVC," << _MSC_FULL_VER << '\n';
    std::wcout << L"GPU," << desc.Description << L",dedicated_video_bytes," << desc.DedicatedVideoMemory << L'\n';
}

struct Window {
    HWND handle = CreateWindowExW(0, L"STATIC", L"SRI UI benchmark", WS_OVERLAPPEDWINDOW,
                                 0, 0, 1280, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~Window() { if (handle) DestroyWindow(handle); }
};

struct GpuTimer {
    ID3D11DeviceContext* context;
    ComPtr<ID3D11Query> disjoint, begin, end;
    explicit GpuTimer(RenderingContext& graphics) : context(graphics.GetDeviceContext()) {
        D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        Require(SUCCEEDED(graphics.GetDevice()->CreateQuery(&desc, disjoint.GetAddressOf())), "disjoint query");
        desc.Query = D3D11_QUERY_TIMESTAMP;
        Require(SUCCEEDED(graphics.GetDevice()->CreateQuery(&desc, begin.GetAddressOf())), "begin query");
        Require(SUCCEEDED(graphics.GetDevice()->CreateQuery(&desc, end.GetAddressOf())), "end query");
    }
    void Begin() { context->Begin(disjoint.Get()); context->End(begin.Get()); }
    void End() { context->End(end.Get()); context->End(disjoint.Get()); }
    template<class T> void Read(ID3D11Query* query, T& value) {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        HRESULT result;
        while ((result = context->GetData(query, &value, sizeof(value), 0)) == S_FALSE) {
            Require(Clock::now() < deadline, "GPU timestamp timeout");
            Sleep(0);
        }
        Require(SUCCEEDED(result), "GPU timestamp read failed");
    }
    double Milliseconds() {
        context->Flush();
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
        UINT64 start = 0, stop = 0;
        Read(disjoint.Get(), frequency); Read(begin.Get(), start); Read(end.Get(), stop);
        Require(!frequency.Disjoint && frequency.Frequency != 0, "Disjoint GPU timestamps; rerun benchmark");
        return static_cast<double>(stop - start) * 1000.0 / static_cast<double>(frequency.Frequency);
    }
};

void Payload() {
    wchar_t executable[MAX_PATH]{};
    Require(GetModuleFileNameW(nullptr, executable, MAX_PATH) != 0, "executable path");
    const auto directory = std::filesystem::path(executable).parent_path();
    for (const auto& file : std::filesystem::directory_iterator(directory)) {
        const auto extension = file.path().extension();
        if (file.is_regular_file() && (extension == ".dll" || file.path().filename() == "ExampleGame.exe"))
            std::cout << "payload," << file.path().filename().string() << ",bytes," << file.file_size() << '\n';
    }
    const auto font = directory / "engine/ui/fonts/DejaVuSans.ttf";
    if (std::filesystem::is_regular_file(font))
        std::cout << "payload,engine/ui/fonts/DejaVuSans.ttf,bytes," << std::filesystem::file_size(font) << '\n';
}
} // namespace

int main(int argc, char** argv) try {
    const bool noUi = argc == 2 && std::string_view(argv[1]) == "--no-ui";
    Require(argc == 1 || noUi, "Usage: ui_benchmark [--no-ui]");
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "configuration," << SRI_BENCHMARK_CONFIG << ",surface,1280x720,warmup," << warmup
              << ",samples," << samples << ",present,false\n";
    Memory("process-start");
    Window window;
    Require(window.handle != nullptr, "CreateWindowExW failed");
    AssetPathResolver::SetGameRoot(std::filesystem::path(SRI_SOURCE_ROOT) / "example_game");
    auto host = std::make_unique<Engine>(EngineConfig{.surface = {window.handle, 1280, 720}, .taskThreads = 1});
    auto* input = host->AddSubsystem<InputSubsystem>();
    auto* rendering = host->AddSubsystem<RenderingSubsystem>();
    auto* scripts = host->AddSubsystem<ScriptSubsystem>();
    Require(host->Initialize(), "Engine initialization failed");
    auto& graphics = rendering->GetContext();
    Machine(graphics);
    ComPtr<ID3D11Texture2D> backbuffer;
    ComPtr<ID3D11RenderTargetView> target;
    Require(SUCCEEDED(graphics.GetSwapChain()->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf()))), "backbuffer");
    Require(SUCCEEDED(graphics.GetDevice()->CreateRenderTargetView(backbuffer.Get(), nullptr, target.GetAddressOf())), "RTV");
    GpuTimer timer(graphics);
    Memory("before-ui");
    Payload();
    if (noUi) { host->Shutdown(); return 0; }
    // Engine explicitly supports late subsystem registration, giving an in-process baseline.
    auto* ui = host->AddSubsystem<UiSubsystem>();
    Require(ui && ui->IsInitialized(), "UI initialization failed");
    Memory("runtime-ready");
    std::string error;
    const auto* entry = host->GetAssets().GetCatalog().FindPath("game/scripts/ui/example_ui.lua");
    Require(entry != nullptr, "Shipped ExampleUI autoload missing from catalog");
    const std::vector<AutoloadDefinition> definitions{{"ExampleUI", {entry->metadata.id, entry->path}, true}};
    Require(scripts->BeginPlay(AssetPathResolver::GetGameRoot(), definitions, error), error);
    const auto stats = UiRuntimeAccess::Inspect(*ui);
    Require(stats.documents == 3 && stats.models == 3 && stats.subscriptions == 4, "Example content incomplete");
    FrameContext frame{.realDeltaSeconds = 1.0 / 60.0, .deltaSeconds = 1.0 / 60.0};
    auto update = [&] {
        ++frame.frameIndex;
        frame.realTimeSeconds += frame.realDeltaSeconds;
        frame.paused = host->IsPaused();
        frame.deltaSeconds = frame.paused ? 0 : frame.realDeltaSeconds;
        frame.simulationTimeSeconds += frame.deltaSeconds;
        input->Update(frame);
        auto start = Clock::now();
        ui->Update(frame);
        double uiMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        scripts->Update(frame);
        start = Clock::now();
        ui->LateUpdate(frame);
        uiMs += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        return uiMs;
    };
    auto press = [&](PhysicalKey key) {
        input->OnPhysicalKey(key, true); update();
        input->OnPhysicalKey(key, false); update();
    };
    auto measure = [&](const char* stage) {
        double updateMs = 0, uiUpdateMs = 0, submitMs = 0, gpuMs = 0;
        for (int i = -warmup; i < samples; ++i) {
            auto start = Clock::now();
            const double uiCpuUpdate = update();
            const double cpuUpdate = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            auto* dc = graphics.GetDeviceContext();
            dc->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
            const float clear[4]{0.03f, 0.03f, 0.03f, 1};
            dc->ClearRenderTargetView(target.Get(), clear);
            timer.Begin();
            start = Clock::now();
            UiRuntimeAccess::RenderOverlay(*ui, graphics, frame);
            const double cpuSubmit = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            timer.End();
            const double gpu = timer.Milliseconds(); // Outside CPU timings; no Present/v-sync.
            if (i >= 0) {
                updateMs += cpuUpdate; uiUpdateMs += uiCpuUpdate; submitMs += cpuSubmit; gpuMs += gpu;
            }
        }
        Memory(stage);
        std::cout << "timing," << stage << ",update_ms," << updateMs / samples
                  << ",ui_update_ms," << uiUpdateMs / samples
                  << ",render_submit_ms," << submitMs / samples << ",gpu_ms," << gpuMs / samples << '\n';
    };
    measure("hud");
    press(PhysicalKey::I);
    InputUiEvent pointer;
    pointer.pointerX = 640;
    pointer.pointerY = 200;
    pointer.hasPointerPosition = true;
    pointer.payload = InputUiPointerMove{};
    input->OnUiEvent(std::move(pointer));
    update();
    Require(ui->GetCaptureState().pointer, "Inventory action did not open UI");
    measure("inventory");
    press(PhysicalKey::P);
    Require(host->IsPaused(), "Pause action did not pause game");
    measure("pause");
    scripts->EndPlay();
    graphics.GetDeviceContext()->ClearState();
    target.Reset(); backbuffer.Reset();
    host->Shutdown();
    return 0;
} catch (const std::exception& exception) {
    std::cerr << "ui_benchmark failed: " << exception.what() << '\n';
    return 1;
}
