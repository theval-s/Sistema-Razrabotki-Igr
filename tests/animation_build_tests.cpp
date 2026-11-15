#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/project/Project.hpp>

#include <gtest/gtest.h>

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
                              const fs::path& output, const fs::path& runtime) {
    std::wstring command = QuoteArgument(tool.native()) + L" --project " + QuoteArgument(project.native()) +
        L" --out " + QuoteArgument(output.native()) + L" --runtime " + QuoteArgument(runtime.native());
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
    fs::create_directories(runtime);
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
    // sri-build checks only that this file exists; this test validates staging, not execution.
    ASSERT_TRUE(engine::WriteFileAtomic(runtime / L"sri_runtime.exe", "runtime marker", error)) << error;
    const fs::path tool = engine::AssetPathResolver::GetRoot() / L"sri-build.exe";
    ASSERT_TRUE(fs::is_regular_file(tool));

    const auto validExit = RunBuild(tool, project.file, output, runtime);
    ASSERT_TRUE(validExit.has_value());
    ASSERT_EQ(*validExit, 0u);
    const fs::path stagedBot = output / L"game" / L"models" / L"TestBot";
    for (const auto* name : {"model.glb", "model.glb.meta", "model.skeleton.json", "model.skeleton.json.meta",
                             "model.skinned-mesh.json", "model.skinned-mesh.json.meta", "idle.animation.json",
                             "idle.animation.json.meta", "walk.animation.json", "walk.animation.json.meta"}) {
        EXPECT_TRUE(fs::is_regular_file(stagedBot / name)) << name;
    }
    EXPECT_TRUE(fs::is_regular_file(output / L"game" / L"scenes" / L"animation.scene.json"));
    EXPECT_TRUE(fs::is_regular_file(output / L"game" / L"scenes" / L"animation.scene.json.meta"));

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
