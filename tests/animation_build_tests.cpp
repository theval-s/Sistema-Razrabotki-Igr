#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetCooking.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/assets/ParticleEffectDefinition.hpp>
#include <engine/scene/SceneDocument.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/EngineShaders.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/core/Content.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/Pak.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/project/Project.hpp>
#include <engine/project/RuntimeSettings.hpp>

#include <gtest/gtest.h>

#include "content_fixture.hpp"

#include <filesystem>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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
                              const fs::path& output, const fs::path& runtime, const std::wstring& extra = {},
                              const fs::path& log = {}) {
    std::wstring command = QuoteArgument(tool.native()) + L" --project " + QuoteArgument(project.native()) +
        L" --out " + QuoteArgument(output.native()) + L" --runtime " + QuoteArgument(runtime.native()) + extra;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE nullDevice = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullDevice == INVALID_HANDLE_VALUE) {
        ADD_FAILURE() << "Opening NUL failed: " << GetLastError();
        return std::nullopt;
    }
    HANDLE outputDevice = nullDevice;
    if (!log.empty()) {
        outputDevice = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (outputDevice == INVALID_HANDLE_VALUE) {
            const DWORD logError = GetLastError();
            CloseHandle(nullDevice);
            ADD_FAILURE() << "Opening build log failed: " << logError;
            return std::nullopt;
        }
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullDevice;
    startup.hStdOutput = startup.hStdError = outputDevice;
    PROCESS_INFORMATION process{};
    // The child inherits the process error mode, suppressing Windows error dialogs.
    const UINT previousMode = SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS |
                                           SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const BOOL created = CreateProcessW(tool.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &process);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    SetErrorMode(previousMode);
    if (outputDevice != nullDevice) CloseHandle(outputDevice);
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

std::string BuildLog(const fs::path& path) {
    std::string text, error;
    return engine::ReadTextFile(path, text, error) ? text : error;
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

TEST(ParticleEffectBuild, CopiesJsonAndValidatesSceneAndTextureUuidDependencies) {
    TemporaryDirectory temp;
    const auto content = temp.path / "project/content";
    const auto runtime = temp.path / "runtime";
    const auto output = temp.path / "output";
    fs::create_directories(content);
    fs::create_directories(runtime);
    std::string error, text;
    engine::Project project;
    project.file = temp.path / "project/Test.sriproject";
    project.name = "ParticleEffectBuildTest";
    project.startupScene = "game/test.scene.json";
    ASSERT_TRUE(project.Save(error)) << error;
    ASSERT_TRUE(engine::WriteFileAtomic(runtime / "sri_runtime.exe", "runtime marker", error)) << error;
    const engine::AssetReference effect{engine::NewUuid(), "game/smoke.particle-effect.json"};
    const engine::AssetReference texture{engine::NewUuid(), "game/smoke.png"};
    engine::ParticleEffectDefinition definition;
    ASSERT_TRUE(engine::SerializeParticleEffectDefinition(definition, text, error)) << error;
    ASSERT_TRUE(engine::WriteFileAtomic(content / "smoke.particle-effect.json", text, error)) << error;
    ASSERT_TRUE(engine::WriteAssetMetadata(content / "smoke.particle-effect.json",
        {effect.id, engine::CatalogAssetKind::ParticleEffect}, error)) << error;
    // Image decoding belongs to runtime texture loading; build validates catalog dependencies and copies bytes.
    ASSERT_TRUE(engine::WriteFileAtomic(content / "smoke.png", "image marker", error)) << error;
    ASSERT_TRUE(engine::WriteAssetMetadata(content / "smoke.png",
        {texture.id, engine::CatalogAssetKind::Texture}, error)) << error;
    engine::SceneDocument scene;
    engine::SceneEntityDefinition entity;
    entity.id = engine::EntityUuid(engine::NewUuid());
    entity.name = "Smoke";
    entity.particleEmitter = engine::ParticleEmitter{};
    entity.particleEmitter->effect = effect;
    scene.entities.push_back(entity);
    const auto saveScene = [&] {
        return engine::SerializeSceneDocument(scene, text, error) &&
            engine::WriteFileAtomic(content / "test.scene.json", text, error);
    };
    const auto saveEffect = [&] {
        return engine::SerializeParticleEffectDefinition(definition, text, error) &&
            engine::WriteFileAtomic(content / "smoke.particle-effect.json", text, error);
    };
    ASSERT_TRUE(saveScene()) << error;
    ASSERT_TRUE(engine::WriteAssetMetadata(content / "test.scene.json",
        {engine::NewUuid(), engine::CatalogAssetKind::Scene}, error)) << error;
    const auto tool = engine::AssetPathResolver::GetRoot() / "sri-build.exe";
    const auto valid = RunBuild(tool, project.file, output, runtime);
    ASSERT_TRUE(valid);
    ASSERT_EQ(*valid, 0u);
    auto pack = engine::PakSource::Load(output / engine::PakFileName, error);
    ASSERT_TRUE(pack) << error;
    EXPECT_TRUE(pack->IsFile(effect.path));
    EXPECT_TRUE(pack->IsFile(texture.path));
    {
        MountScope mount(output, pack);
        struct Reset {
            ~Reset() { engine::AssetPathResolver::SetManifest({}); engine::AssetPathResolver::SetGameRoot({}); }
        } reset;
        engine::AssetPathResolver::SetManifest(output / "manifest.json");
        engine::AssetPathResolver::SetGameRoot(output / "game");
        engine::Engine host{engine::EngineConfig{.taskThreads = 1}};
        ASSERT_TRUE(host.Initialize());
        auto& assets = host.GetSceneAssets();
        ASSERT_NE(assets.GetCatalog().Find(effect.id, engine::CatalogAssetKind::ParticleEffect), nullptr);
        EXPECT_TRUE(assets.LoadParticleEffect(effect.path));
    }
    pack.reset();
    ASSERT_TRUE(engine::WriteFileAtomic(output / "preserve.txt", "preserve", error)) << error;
    const auto rejected = [&] {
        const auto status = RunBuild(tool, project.file, output, runtime);
        ASSERT_TRUE(status);
        EXPECT_NE(*status, 0u);
        std::string marker;
        ASSERT_TRUE(engine::ReadTextFile(output / "preserve.txt", marker, error)) << error;
        EXPECT_EQ(marker, "preserve");
    };
    scene.entities[0].particleEmitter->effect = {texture.id, texture.path};
    ASSERT_TRUE(saveScene()) << error;
    rejected(); // scene effect is a texture UUID
    scene.entities[0].particleEmitter->effect = effect;
    ASSERT_TRUE(saveScene()) << error;
    definition.texture = {effect.id, effect.path};
    ASSERT_TRUE(saveEffect()) << error;
    rejected(); // effect texture is an effect UUID
    definition.texture = {engine::NewUuid(), "game/missing.png"};
    ASSERT_TRUE(saveEffect()) << error;
    rejected();
    definition.texture = texture;
    ASSERT_TRUE(saveEffect()) << error;
    const auto textured = RunBuild(tool, project.file, output, runtime, L" --no-pak");
    ASSERT_TRUE(textured);
    EXPECT_EQ(*textured, 0u);
    EXPECT_TRUE(fs::is_regular_file(output / "game/smoke.particle-effect.json"));
    EXPECT_TRUE(fs::is_regular_file(output / "game/smoke.png"));
}

TEST(SkyboxBuild, PackagesLooseAndPackedCubemapShadersAndPreservesOutputOnValidationFailure) {
    TemporaryDirectory temp;
    const auto projectRoot = temp.path / "project";
    const auto content = projectRoot / "content";
    const auto runtime = temp.path / "runtime";
    const auto loose = temp.path / "loose";
    const auto output = temp.path / "packed";
    const auto log = temp.path / "build.log";
    fs::create_directories(content / "materials");
    fs::create_directories(runtime);
    const auto fixture = fs::path(SRI_SOURCE_ROOT) / "example_game/materials";
    for (const auto* name : {"example-sky.dds", "example-sky.dds.meta",
                            "example-sky.skybox.json", "example-sky.skybox.json.meta"}) {
        fs::copy_file(fixture / name, content / "materials" / name);
    }
    std::string error, text;
    engine::AssetMetadata skyMetadata, textureMetadata;
    const auto skyFile = content / "materials/example-sky.skybox.json";
    const auto ddsFile = content / "materials/example-sky.dds";
    ASSERT_TRUE(engine::ReadAssetMetadata(skyFile, skyMetadata, error)) << error;
    ASSERT_TRUE(engine::ReadAssetMetadata(ddsFile, textureMetadata, error)) << error;
    const engine::AssetReference sky{skyMetadata.id, "game/materials/example-sky.skybox.json"};
    const engine::AssetReference cubemap{textureMetadata.id, "game/materials/example-sky.dds"};
    engine::SceneDocument scene;
    scene.environment.skybox = sky;
    const auto saveScene = [&] {
        return engine::SerializeSceneDocument(scene, text, error) &&
            engine::WriteFileAtomic(content / "test.scene.json", text, error);
    };
    ASSERT_TRUE(saveScene()) << error;
    ASSERT_TRUE(engine::WriteAssetMetadata(content / "test.scene.json",
        {engine::NewUuid(), engine::CatalogAssetKind::Scene}, error)) << error;
    engine::Project project;
    project.file = projectRoot / "Skybox.sriproject";
    project.name = "SkyboxBuildTest";
    project.startupScene = "game/test.scene.json";
    ASSERT_TRUE(project.Save(error)) << error;
    ASSERT_TRUE(engine::WriteFileAtomic(runtime / "sri_runtime.exe", "runtime marker", error)) << error;
    const auto tool = engine::AssetPathResolver::GetRoot() / "sri-build.exe";
    ASSERT_TRUE(fs::is_regular_file(tool));
    std::vector<std::uint8_t> sourceDds;
    ASSERT_TRUE(engine::ReadBinaryFile(ddsFile, sourceDds, error)) << error;

    const auto checkContent = [&](const fs::path& root) {
        std::string manifest, descriptor;
        ASSERT_TRUE(engine::ReadTextFile(root / "manifest.json", manifest, error)) << error;
        engine::AssetCatalog catalog;
        ASSERT_TRUE(catalog.LoadManifest(manifest, root / "game", root / "engine", error)) << error;
        ASSERT_NE(catalog.Find(sky.id, engine::CatalogAssetKind::Skybox), nullptr);
        ASSERT_NE(catalog.Find(cubemap.id, engine::CatalogAssetKind::Texture), nullptr);
        EXPECT_EQ(catalog.Find(sky.id)->path, sky.path);
        EXPECT_EQ(catalog.Find(cubemap.id)->path, cubemap.path);
        ASSERT_TRUE(engine::ReadTextFile(root / "game/test.scene.json", descriptor, error)) << error;
        engine::SceneDocument shippedScene;
        ASSERT_TRUE(engine::ParseSceneDocument(descriptor, shippedScene, error)) << error;
        EXPECT_EQ(shippedScene.environment.skybox, sky);
        ASSERT_TRUE(engine::ReadTextFile(root / sky.path, descriptor, error)) << error;
        engine::SkyboxDefinition definition;
        ASSERT_TRUE(engine::ParseSkyboxDefinition(descriptor, definition, error)) << error;
        EXPECT_EQ(definition.cubemap.id, cubemap.id);
        std::vector<std::uint8_t> bytes;
        ASSERT_TRUE(engine::ReadBinaryFile(root / cubemap.path, bytes, error)) << error;
        EXPECT_EQ(bytes, sourceDds);
        ASSERT_TRUE(engine::ReadBinaryFile(root / engine::ShaderLibraryName, bytes, error)) << error;
        engine::ShaderLibrary shaders;
        ASSERT_TRUE(shaders.Deserialize(bytes, error)) << error;
        for (const auto shader : {engine::EngineShader::SkyboxVS, engine::EngineShader::SkyboxPS}) {
            const auto& variant = engine::GetEngineShader(shader);
            const auto file = engine::AssetPathResolver::ResolveShader({.path = engine::AssetPathResolver::DefaultPath(
                engine::ToUtf8(variant.file), engine::AssetKind::Shader)});
            const auto* compiled = shaders.Find(engine::ShaderLibrary::Key(file, variant.stage, variant.options));
            ASSERT_NE(compiled, nullptr) << static_cast<unsigned>(shader);
            EXPECT_FALSE(compiled->empty());
        }
    };

    const auto looseStatus = RunBuild(tool, project.file, loose, runtime, L" --no-pak", log);
    ASSERT_TRUE(looseStatus);
    ASSERT_EQ(*looseStatus, 0u) << BuildLog(log);
    EXPECT_FALSE(fs::exists(loose / engine::PakFileName));
    ASSERT_NO_FATAL_FAILURE(checkContent(loose));
    const auto packedStatus = RunBuild(tool, project.file, output, runtime, {}, log);
    ASSERT_TRUE(packedStatus);
    ASSERT_EQ(*packedStatus, 0u) << BuildLog(log);
    EXPECT_TRUE(fs::is_regular_file(output / "SkyboxBuildTest.exe"));
    EXPECT_FALSE(fs::exists(output / "game"));
    EXPECT_FALSE(fs::exists(output / "manifest.json"));
    {
        auto pack = engine::PakSource::Load(output / engine::PakFileName, error);
        ASSERT_TRUE(pack) << error;
        EXPECT_TRUE(pack->IsFile(sky.path));
        EXPECT_TRUE(pack->IsFile(cubemap.path));
        EXPECT_FALSE(pack->IsFile(sky.path + ".meta"));
        EXPECT_FALSE(pack->IsFile(cubemap.path + ".meta"));
        MountScope mount(output, std::move(pack));
        ASSERT_NO_FATAL_FAILURE(checkContent(output));
    }

    // Validation must precede Stage's replacement of the previous good output.
    std::vector<std::uint8_t> goodPack;
    ASSERT_TRUE(engine::ReadBinaryFile(output / engine::PakFileName, goodPack, error)) << error;
    ASSERT_TRUE(engine::WriteFileAtomic(output / "preserve.txt", "previous good skybox build", error)) << error;
    const auto rejected = [&](std::string_view diagnostic) {
        const auto status = RunBuild(tool, project.file, output, runtime, {}, log);
        ASSERT_TRUE(status);
        EXPECT_NE(*status, 0u) << BuildLog(log);
        EXPECT_NE(BuildLog(log).find(diagnostic), std::string::npos) << BuildLog(log);
        std::string marker;
        ASSERT_TRUE(engine::ReadTextFile(output / "preserve.txt", marker, error)) << error;
        EXPECT_EQ(marker, "previous good skybox build");
        std::vector<std::uint8_t> retained;
        ASSERT_TRUE(engine::ReadBinaryFile(output / engine::PakFileName, retained, error)) << error;
        EXPECT_EQ(retained, goodPack);
    };
    // Well-formed DX10 1x1 RGBA Texture2D DDS, with no cubemap flag.
    std::string flatDds(152, '\0');
    const auto word = [&](unsigned offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) flatDds[offset + i] = static_cast<char>(value >> (8 * i));
    };
    word(0, 0x20534444); word(4, 124); word(8, 0x100f);
    word(12, 1); word(16, 1); word(20, 4); word(28, 1);
    word(76, 32); word(80, 4); word(84, 0x30315844); word(108, 0x1000);
    word(128, 28); word(132, 3); word(140, 1);
    ASSERT_TRUE(engine::WriteFileAtomic(ddsFile, flatDds, error)) << error;
    ASSERT_NO_FATAL_FAILURE(rejected("cubemap"));
    ASSERT_TRUE(engine::WriteFileAtomic(ddsFile,
        std::string_view(reinterpret_cast<const char*>(sourceDds.data()), sourceDds.size()), error)) << error;
    scene.environment.skybox = {engine::NewUuid(), sky.path};
    ASSERT_TRUE(saveScene()) << error;
    ASSERT_NO_FATAL_FAILURE(rejected("environment.skybox"));

    // Consume the retained pack through the runtime path, with no loose sources.
    auto pack = engine::PakSource::Load(output / engine::PakFileName, error);
    ASSERT_TRUE(pack) << error;
    MountScope mount(output, std::move(pack));
    struct RestoreRoots {
        fs::path game = engine::AssetPathResolver::GetGameRoot();
        fs::path manifest = engine::AssetPathResolver::GetManifest();
        ~RestoreRoots() {
            engine::AssetPathResolver::SetManifest(manifest);
            engine::AssetPathResolver::SetGameRoot(game);
        }
    } restore;
    engine::AssetPathResolver::SetManifest(output / "manifest.json");
    engine::AssetPathResolver::SetGameRoot(output / "game");
    struct HiddenWindow {
        HWND handle = CreateWindowExW(0, L"STATIC", L"Packed skybox build test", WS_OVERLAPPEDWINDOW,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~HiddenWindow() { if (handle) DestroyWindow(handle); }
    } window;
    ASSERT_NE(window.handle, nullptr);
    engine::Engine host({.surface = {window.handle, 64, 64}, .taskThreads = 1});
    auto* rendering = host.AddSubsystem<engine::RenderingSubsystem>();
    if (!host.Initialize()) GTEST_SKIP() << "Packaging checks passed; D3D11 hardware/debug layer unavailable";
    auto& assets = host.GetSceneAssets();
    ASSERT_TRUE(assets.GetCatalogError().empty()) << assets.GetCatalogError();
    const auto handle = assets.RequestSkybox(sky);
    const auto loaded = assets.LoadSkybox(sky.path);
    ASSERT_TRUE(loaded);
    EXPECT_TRUE(assets.IsReady(handle));
    ASSERT_TRUE(assets.IsReady(loaded->cubemap));
    const auto texture = assets.Peek(loaded->cubemap);
    ASSERT_TRUE(texture);
    const auto* native = rendering->GetResourceManager().TryGetTexture(texture->texture);
    ASSERT_NE(native, nullptr);
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    native->GetSRV()->GetDesc(&srv);
    EXPECT_EQ(srv.ViewDimension, D3D11_SRV_DIMENSION_TEXTURECUBE);
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    native->GetSRV()->GetResource(resource.GetAddressOf());
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cube;
    ASSERT_TRUE(SUCCEEDED(resource.As(&cube)));
    D3D11_TEXTURE2D_DESC desc{};
    cube->GetDesc(&desc);
    EXPECT_EQ(desc.Width, desc.Height);
    EXPECT_GT(desc.Width, 0u);
    EXPECT_EQ(desc.ArraySize, 6u);
    EXPECT_EQ(desc.SampleDesc.Count, 1u);
    EXPECT_NE(desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE, 0u);
}
