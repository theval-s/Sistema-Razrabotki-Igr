#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetCooking.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/EngineShaders.hpp>
#include <engine/core/Content.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/Pak.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/project/Project.hpp>
#include <engine/project/RuntimeSettings.hpp>

#include <gtest/gtest.h>

#include "content_fixture.hpp"

#include <filesystem>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace {
namespace fs = std::filesystem;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto base = fs::temp_directory_path();
        for (unsigned int i = 0; i != 100; ++i) {
            auto candidate = base / (L"sri animation build \u0442\u0435\u0441\u0442 " +
                std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(GetTickCount64()) +
                L" " + std::to_wstring(i));
            std::error_code ec;
            if (fs::create_directory(candidate, ec)) {
                path = std::move(candidate);
                return;
            }
            if (ec) throw fs::filesystem_error("create temporary directory", candidate, ec);
        }
        throw std::runtime_error("could not allocate a unique temporary directory");
    }
    ~TemporaryDirectory() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    fs::path path;
};

// Windows CRT command-line quoting, including backslashes before quotes and the closing quote.
std::wstring QuoteArgument(const std::wstring& argument) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++slashes;
        } else if (ch == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result += ch;
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            result += ch;
            slashes = 0;
        }
    }
    result.append(slashes * 2, L'\\');
    return result + L"\"";
}

std::optional<DWORD> RunBuild(const fs::path& tool, const fs::path& project,
                              const fs::path& output, const fs::path& runtime, const std::wstring& extra = {}) {
    std::wstring command = QuoteArgument(tool.native()) + L" --project " + QuoteArgument(project.native()) +
        L" --out " + QuoteArgument(output.native()) + L" --runtime " + QuoteArgument(runtime.native()) + extra;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE nullDevice = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullDevice == INVALID_HANDLE_VALUE) {
        ADD_FAILURE() << "Opening NUL failed: " << GetLastError();
        return std::nullopt;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = startup.hStdOutput = startup.hStdError = nullDevice;
    PROCESS_INFORMATION process{};
    // The child inherits the process error mode, suppressing Windows error dialogs.
    const UINT previousMode = SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS |
                                           SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const BOOL created = CreateProcessW(tool.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &process);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    SetErrorMode(previousMode);
    CloseHandle(nullDevice);
    if (!created) {
        ADD_FAILURE() << "CreateProcessW failed: " << createError;
        return std::nullopt;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    if (wait != WAIT_OBJECT_0) {
        const DWORD waitError = wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hProcess);
        ADD_FAILURE() << "sri-build did not complete within 30 seconds (wait=" << wait
                      << ", error=" << waitError << ")";
        return std::nullopt;
    }
    DWORD exitCode = 0;
    const BOOL gotExitCode = GetExitCodeProcess(process.hProcess, &exitCode);
    const DWORD exitError = gotExitCode ? ERROR_SUCCESS : GetLastError();
    CloseHandle(process.hProcess);
    if (!gotExitCode) {
        ADD_FAILURE() << "GetExitCodeProcess failed: " << exitError;
        return std::nullopt;
    }
    return exitCode;
}
} // namespace

TEST(AnimationBuild, ValidatesBeforeReplacingStagedOutput) {
    TemporaryDirectory temp;
    const fs::path projectRoot = temp.path / L"project";
    const fs::path content = projectRoot / L"content";
    const fs::path runtime = temp.path / L"runtime";
    const fs::path output = temp.path / L"output";
    const fs::path fixture = fs::path(SRI_SOURCE_ROOT) / "example_game";
    fs::create_directories(content / L"models");
    fs::create_directories(content / L"scenes");
    fs::create_directories(runtime / L"bin");
    fs::copy(fixture / "models" / "TestBot", content / L"models" / L"TestBot", fs::copy_options::recursive);
    for (const auto* name : {"animation.scene.json", "animation.scene.json.meta"}) {
        fs::copy_file(fixture / "scenes" / name, content / L"scenes" / name);
    }

    engine::Project project;
    project.file = projectRoot / L"Test.sriproject";
    project.name = "AnimationBuildTest";
    project.startupScene = "game/scenes/animation.scene.json";
    std::string error;
    ASSERT_TRUE(project.Save(error)) << error;
    // sri-build checks only that the launcher exists; this test validates staging, not execution.
    ASSERT_TRUE(engine::WriteFileAtomic(runtime / L"sri_runtime.exe", "runtime marker", error)) << error;
    ASSERT_TRUE(engine::WriteFileAtomic(runtime / L"bin" / L"sri_game.dll", "library marker", error)) << error;
    const fs::path tool = engine::AssetPathResolver::GetRoot() / L"sri-build.exe";
    ASSERT_TRUE(fs::is_regular_file(tool));

    const std::vector<std::string> botFiles{"model.glb", "model.skeleton.json", "model.skinned-mesh.json",
                                            "idle.animation.json", "walk.animation.json"};

    // --no-pak keeps the cooked layout loose beside the executable.
    const fs::path loose = temp.path / L"loose";
    const auto looseExit = RunBuild(tool, project.file, loose, runtime, L" --no-pak");
    ASSERT_TRUE(looseExit.has_value());
    ASSERT_EQ(*looseExit, 0u);
    EXPECT_FALSE(fs::exists(loose / engine::PakFileName));
    for (const auto& name : botFiles) {
        const auto source = loose / L"game" / L"models" / L"TestBot" / engine::FromUtf8(name);
        EXPECT_TRUE(fs::is_regular_file(engine::CookedPath(source))) << name;
        EXPECT_FALSE(fs::exists(source)) << name;
    }
    EXPECT_TRUE(fs::is_regular_file(loose / L"manifest.json"));
    EXPECT_TRUE(fs::is_regular_file(loose / engine::ShaderLibraryName));

    const auto validExit = RunBuild(tool, project.file, output, runtime);
    ASSERT_TRUE(validExit.has_value());
    ASSERT_EQ(*validExit, 0u);
    // Only the launcher, its libraries, the runtime settings and the pack are left on disk.
    EXPECT_TRUE(fs::is_regular_file(output / L"AnimationBuildTest.exe"));
    EXPECT_TRUE(fs::is_regular_file(output / L"bin" / L"sri_game.dll"));
    EXPECT_TRUE(fs::is_regular_file(output / engine::RuntimeSettings::FileName));
    EXPECT_FALSE(fs::exists(output / L"game"));
    EXPECT_FALSE(fs::exists(output / L"engine"));
    EXPECT_FALSE(fs::exists(output / L"manifest.json"));
    auto pak = engine::PakSource::Load(output / engine::PakFileName, error);
    ASSERT_TRUE(pak) << error;
    const auto files = pak->Files();
    for (const auto& file : files) {
        EXPECT_FALSE(file.ends_with(".meta")) << file;
        EXPECT_FALSE(file.ends_with(".hlsl")) << file;
    }
    for (const auto& name : botFiles) {
        EXPECT_TRUE(pak->IsFile("game/models/TestBot/" + name + ".cooked")) << name;
        EXPECT_FALSE(pak->IsFile("game/models/TestBot/" + name)) << name;
    }
    for (const auto* name : {"game/scenes/animation.scene.json", "manifest.json", "shaders.cooked", "game.sriproject",
                             "engine/primitives/cube/model.obj.cooked"}) {
        EXPECT_TRUE(pak->IsFile(name)) << name;
    }

    MountScope mount(output, std::move(pak));
    {
        const auto packedProject = engine::Project::Load(output / L"game.sriproject", error);
        ASSERT_TRUE(packedProject) << error;
        EXPECT_EQ(packedProject->startupScene, project.startupScene);
        std::vector<std::uint8_t> bytes;
        ASSERT_TRUE(engine::ReadBinaryFile(output / engine::ShaderLibraryName, bytes, error)) << error;
        engine::ShaderLibrary library;
        ASSERT_TRUE(library.Deserialize(bytes, error)) << error;
        for (std::size_t i = 0; i < static_cast<std::size_t>(engine::EngineShader::Count); ++i) {
            const auto& variant = engine::GetEngineShader(static_cast<engine::EngineShader>(i));
            const auto file = engine::AssetPathResolver::ResolveShader({.path = engine::AssetPathResolver::DefaultPath(
                engine::ToUtf8(variant.file), engine::AssetKind::Shader)});
            EXPECT_NE(library.Find(engine::ShaderLibrary::Key(file, variant.stage, variant.options)), nullptr) << i;
        }
    }

    {
        // The packed build runs without any source or .meta file.
        engine::AssetPathResolver::SetManifest(output / L"manifest.json");
        engine::AssetPathResolver::SetGameRoot(output / L"game");
        struct Restore {
            ~Restore() {
                engine::AssetPathResolver::SetManifest({});
                engine::AssetPathResolver::SetGameRoot({});
            }
        } restore;
        engine::Engine host{engine::EngineConfig{.taskThreads = 1}};
        ASSERT_TRUE(host.Initialize());
        auto& assets = host.GetSceneAssets();
        EXPECT_TRUE(assets.GetCatalogError().empty()) << assets.GetCatalogError();
        EXPECT_NE(assets.GetCatalog().FindPath("game/models/TestBot/model.glb"), nullptr);
        const auto skeleton = assets.LoadSkeleton("game/models/TestBot/model.skeleton.json");
        const auto walk = assets.LoadAnimationClip("game/models/TestBot/walk.animation.json");
        ASSERT_TRUE(skeleton);
        ASSERT_TRUE(walk);
        EXPECT_EQ(walk->skeletonSignature, skeleton->signature);
        host.Shutdown();
    }
    engine::content::Unmount(output); // let the next build replace the pack

    const fs::path marker = output / L"preserve-marker.txt";
    ASSERT_TRUE(engine::WriteFileAtomic(marker, "preserve this output", error)) << error;
    const fs::path walk = content / L"models" / L"TestBot" / L"walk.animation.json";
    std::string descriptor;
    ASSERT_TRUE(engine::ReadTextFile(walk, descriptor, error)) << error;
    const auto clip = descriptor.find("\"clip\": \"walk\"");
    ASSERT_NE(clip, std::string::npos);
    descriptor.replace(clip, std::string("\"clip\": \"walk\"").size(), "\"clip\": \"nonexistent_clip\"");
    ASSERT_TRUE(engine::WriteFileAtomic(walk, descriptor, error)) << error;
    const auto invalidExit = RunBuild(tool, project.file, output, runtime);
    ASSERT_TRUE(invalidExit.has_value());
    EXPECT_NE(*invalidExit, 0u);
    std::string markerText;
    ASSERT_TRUE(engine::ReadTextFile(marker, markerText, error)) << error;
    EXPECT_EQ(markerText, "preserve this output");
}
