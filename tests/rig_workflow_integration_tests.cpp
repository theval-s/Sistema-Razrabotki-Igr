#include <engine/animation/AnimationSubsystem.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetPipeline.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/assets/MaterialDefinition.hpp>
#include <engine/assets/RigAuthoring.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/engine.hpp>
#include <engine/scene/SceneDocument.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>
#include "animation/AnimationData.hpp"

#include <gtest/gtest.h>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {
using namespace engine;
namespace fs = std::filesystem;

// Declared before the Engine so its destructor restores the global root and
// removes only our disposable tree after all engine resources have gone away.
struct WorkflowRoot {
    fs::path previous = AssetPathResolver::GetGameRoot();
    fs::path root = fs::temp_directory_path() / ("sri-rig-workflow-" + NewUuid());
    RigAuthoringContext context{root / "content", {}, root / ".sri/rig-transactions"};
    WorkflowRoot() { fs::create_directories(context.gameRoot / "models"); AssetPathResolver::SetGameRoot(context.gameRoot); }
    ~WorkflowRoot() {
        AssetPathResolver::SetGameRoot(previous);
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    fs::path Physical(const AssetReference& ref) const { return context.gameRoot / FromUtf8(ref.path.substr(5)); }
};

bool Different(const AnimationPose& a, const AnimationPose& b) {
    if (a.jointModels.size() != b.jointModels.size()) return true;
    for (std::size_t i = 0; i < a.jointModels.size(); ++i)
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                if (std::abs(a.jointModels[i].m[r][c] - b.jointModels[i].m[r][c]) > 1e-4f) return true;
    return false;
}

void RunWorkflow(const fs::path& source) {
    WorkflowRoot temp;
    std::string error;
    RigInspection inspection;
    ASSERT_TRUE(InspectAnimatedGlb(source, inspection, error)) << error;
    ASSERT_TRUE(inspection.importable);
    ASSERT_GT(inspection.jointCount, 0u);
    ASSERT_GT(inspection.sectionCount, 0u);

    // Copy the actual engine material into the disposable catalog with a fresh
    // identity, avoiding collisions with the executable's engine catalog.
    const auto material = temp.context.gameRoot / "base.material.json";
    fs::copy_file(fs::path(SRI_SOURCE_ROOT) / "engine/primitives/default.material.json", material);
    const AssetReference base{NewUuid(), "game/base.material.json"};
    ASSERT_TRUE(WriteAssetMetadata(material, {base.id, CatalogAssetKind::Material}, error)) << error;
    RigImportRequest request;
    request.sourceGlb = source;
    request.destinationDirectory = temp.context.gameRoot / "models/Rig";
    request.expectedSourceRevision = inspection.sourceRevision;
    request.baseMaterial = base;
    request.extractBaseColor = inspection.canExtractBaseColor;
    for (const auto& clip : inspection.clips)
        if (clip.importable) request.clips.push_back(clip.name);
    ASSERT_FALSE(request.clips.empty());
    const auto imported = ImportAnimatedRig(temp.context, request);
    ASSERT_EQ(imported.state, RigPublishState::Committed) << imported.error;
    ASSERT_EQ(imported.group.clips.size(), request.clips.size());
    ASSERT_TRUE(imported.group.materialInstance);
    EXPECT_EQ(imported.group.texture.has_value(), inspection.canExtractBaseColor);

    AssetCatalog catalog;
    ASSERT_TRUE(catalog.Scan(temp.context.gameRoot, {}, error)) << error;
    const auto checkCatalog = [&](const AssetReference& ref, CatalogAssetKind kind) {
        const auto* entry = catalog.FindPath(ref.path);
        EXPECT_NE(entry, nullptr) << ref.path;
        if (entry) { EXPECT_EQ(entry->metadata.kind, kind); EXPECT_EQ(entry->metadata.id, ref.id); }
    };
    checkCatalog(imported.group.source, CatalogAssetKind::Mesh);
    checkCatalog(imported.group.skeleton, CatalogAssetKind::Skeleton);
    checkCatalog(imported.group.mesh, CatalogAssetKind::SkinnedMesh);
    checkCatalog(*imported.group.materialInstance, CatalogAssetKind::MaterialInstance);
    if (imported.group.texture) checkCatalog(*imported.group.texture, CatalogAssetKind::Texture);
    for (const auto& clip : imported.group.clips) checkCatalog(clip.asset, CatalogAssetKind::AnimationClip);

    const auto readCpu = [&](AssetKind kind, const AssetReference& ref, CpuPayload& payload) {
        AssetDesc desc; desc.path = temp.Physical(ref); desc.catalog = &catalog;
        return SkeletalAssetImporter{kind}.ReadCpu(desc, nullptr, payload, error);
    };
    CpuPayload skeleton, mesh;
    ASSERT_TRUE(readCpu(AssetKind::Skeleton, imported.group.skeleton, skeleton)) << error;
    ASSERT_TRUE(readCpu(AssetKind::SkinnedMesh, imported.group.mesh, mesh)) << error;
    ASSERT_TRUE(skeleton.skeleton.data);
    EXPECT_EQ(mesh.skinnedMesh.skeletonSignature, skeleton.skeleton.signature);
    EXPECT_EQ(mesh.skinnedMesh.skeletonReference, imported.group.skeleton);
    ASSERT_FALSE(mesh.skinnedMesh.sections.empty());
    for (const auto& section : mesh.skinnedMesh.sections) {
        ASSERT_TRUE(section.binding);
        ASSERT_FALSE(section.vertices.empty());
        ASSERT_FALSE(section.indices.empty());
        for (auto index : section.indices) EXPECT_LT(index, section.vertices.size());
        for (const auto& vertex : section.vertices) {
            float sum = 0;
            for (std::size_t i = 0; i < vertex.weights.size(); ++i) {
                ASSERT_TRUE(std::isfinite(vertex.weights[i]));
                EXPECT_GE(vertex.weights[i], 0.f);
                sum += vertex.weights[i];
                if (vertex.weights[i] > 0) {
                    ASSERT_LT(vertex.joints[i], section.binding->joints.size());
                    EXPECT_LT(section.binding->joints[vertex.joints[i]], inspection.jointCount);
                }
            }
            EXPECT_NEAR(sum, 1.f, 1e-4f);
        }
    }
    double firstDuration = 0;
    for (const auto& clip : imported.group.clips) {
        CpuPayload cpu;
        ASSERT_TRUE(readCpu(AssetKind::AnimationClip, clip.asset, cpu)) << error;
        ASSERT_TRUE(cpu.animationClip.data);
        EXPECT_EQ(cpu.animationClip.skeletonSignature, skeleton.skeleton.signature);
        EXPECT_EQ(cpu.animationClip.data->animation.num_tracks(), skeleton.skeleton.jointCount);
        EXPECT_GT(cpu.animationClip.duration, 0.f);
        if (firstDuration == 0) firstDuration = cpu.animationClip.duration;
    }

    Engine host{EngineConfig{.taskThreads = 1}};
    host.SetEditorMode();
    auto* animation = host.AddSubsystem<AnimationSubsystem>();
    host.AddSubsystem<ScriptSubsystem>();
    ASSERT_TRUE(host.Initialize());
    auto& assets = host.GetSceneAssets();
    ASSERT_TRUE(assets.LoadSkeleton(imported.group.skeleton.path));
    for (const auto& clip : imported.group.clips) ASSERT_TRUE(assets.LoadAnimationClip(clip.asset.path));
    auto& scene = host.GetScene();
    const auto a = scene.CreateEntity("preview"), b = scene.CreateEntity("stationary");
    const auto clip = imported.group.clips.front().asset;
    scene.AddComponent<Animator>(a, Animator{imported.group.skeleton, clip, 1.f, true, true});
    scene.AddComponent<Animator>(b, Animator{imported.group.skeleton, clip, 1.f, true, true});
    animation->LateUpdate(FrameContext{.realDeltaSeconds = 0, .deltaSeconds = 0});
    ASSERT_TRUE(animation->GetPose(a));
    ASSERT_TRUE(animation->GetPose(b));
    const AnimationPose initial = *animation->GetPose(a), stationary = *animation->GetPose(b);
    SceneDocument before, after;
    ASSERT_TRUE(CaptureSceneDocument(scene, assets, before, error)) << error;
    std::string beforeText, afterText;
    ASSERT_TRUE(SerializeSceneDocument(before, beforeText, error)) << error;
    ASSERT_TRUE(animation->Preview(a, clip, 0, true, error)) << error;
    ASSERT_GT(firstDuration, 0);
    const double step = firstDuration / 8;
    bool moved = false;
    for (int i = 0; i < 4; ++i) {
        animation->LateUpdate(FrameContext{.realDeltaSeconds = step, .deltaSeconds = 0});
        ASSERT_TRUE(animation->GetPose(a));
        moved |= Different(*animation->GetPose(a), initial);
    }
    ASSERT_TRUE(animation->GetPlaybackState(a));
    ASSERT_TRUE(animation->GetPlaybackState(b));
    EXPECT_GT(animation->GetPlaybackState(a)->timeSeconds, 0);
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(b)->timeSeconds, 0);
    EXPECT_TRUE(moved);
    EXPECT_FALSE(Different(*animation->GetPose(b), stationary));
    ASSERT_TRUE(CaptureSceneDocument(scene, assets, after, error)) << error;
    ASSERT_TRUE(SerializeSceneDocument(after, afterText, error)) << error;
    EXPECT_EQ(beforeText, afterText);

    RigReimportRequest reimport;
    reimport.directory = imported.group.directory;
    reimport.sourceGlb = source;
    reimport.expectedSourceRevision = inspection.sourceRevision;
    reimport.expectedGroupRevision = imported.group.revision;
    for (const auto& binding : imported.group.clips) reimport.clips.push_back({binding.asset.id, binding.sourceName, false});
    const auto refreshed = ReimportAnimatedRig(temp.context, reimport);
    ASSERT_EQ(refreshed.state, RigPublishState::Committed) << refreshed.error;
    EXPECT_EQ(refreshed.group.source, imported.group.source);
    EXPECT_EQ(refreshed.group.skeleton, imported.group.skeleton);
    EXPECT_EQ(refreshed.group.mesh, imported.group.mesh);
    ASSERT_EQ(refreshed.group.clips.size(), imported.group.clips.size());
    for (std::size_t i = 0; i < refreshed.group.clips.size(); ++i)
        EXPECT_EQ(refreshed.group.clips[i].asset, imported.group.clips[i].asset);
    const auto oldHandle = assets.RequestSkeleton(imported.group.skeleton);
    scene.Clear();
    animation->ResetSimulation();
    assets.UnloadAll();
    EXPECT_FALSE(assets.Peek(oldHandle));
    ASSERT_TRUE(assets.RescanCatalog(error)) << error;
    ASSERT_TRUE(assets.LoadSkeleton(refreshed.group.skeleton.path));
    ASSERT_TRUE(assets.LoadAnimationClip(refreshed.group.clips.front().asset.path));
    EXPECT_NE(assets.RequestSkeleton(refreshed.group.skeleton), oldHandle);

    const auto duplicate = DuplicateAnimatedRig(temp.context, refreshed.group.directory,
        temp.context.gameRoot / "models/Copy", refreshed.group.revision);
    ASSERT_EQ(duplicate.state, RigPublishState::Committed) << duplicate.error;
    EXPECT_NE(duplicate.group.source.id, refreshed.group.source.id);
    EXPECT_NE(duplicate.group.skeleton.id, refreshed.group.skeleton.id);
    EXPECT_NE(duplicate.group.mesh.id, refreshed.group.mesh.id);
    const auto text = [&](const AssetReference& ref) {
        std::string result;
        EXPECT_TRUE(ReadTextFile(temp.Physical(ref), result, error)) << error;
        return result;
    };
    SkeletonDefinition copiedSkeleton;
    ASSERT_TRUE(ParseSkeletonDefinition(text(duplicate.group.skeleton), copiedSkeleton, error)) << error;
    EXPECT_EQ(copiedSkeleton.source, duplicate.group.source);
    SkinnedMeshDefinition copiedMesh;
    ASSERT_TRUE(ParseSkinnedMeshDefinition(text(duplicate.group.mesh), copiedMesh, error)) << error;
    EXPECT_EQ(copiedMesh.skeleton, duplicate.group.skeleton);
    ASSERT_EQ(duplicate.group.clips.size(), refreshed.group.clips.size());
    for (std::size_t i = 0; i < duplicate.group.clips.size(); ++i) {
        EXPECT_NE(duplicate.group.clips[i].asset.id, refreshed.group.clips[i].asset.id);
        AnimationClipDefinition copiedClip;
        ASSERT_TRUE(ParseAnimationClipDefinition(text(duplicate.group.clips[i].asset), copiedClip, error)) << error;
        EXPECT_EQ(copiedClip.skeleton, duplicate.group.skeleton);
    }
    ASSERT_TRUE(duplicate.group.materialInstance);
    EXPECT_NE(duplicate.group.materialInstance->id, refreshed.group.materialInstance->id);
    MaterialInstanceDefinition instance;
    ASSERT_TRUE(ParseMaterialInstanceDefinition(text(*duplicate.group.materialInstance), instance, error)) << error;
    ASSERT_TRUE(instance.materialReference);
    EXPECT_EQ(*instance.materialReference, base);
    if (refreshed.group.texture) {
        ASSERT_TRUE(duplicate.group.texture);
        EXPECT_NE(duplicate.group.texture->id, refreshed.group.texture->id);
        ASSERT_TRUE(instance.parameters.albedoReference);
        EXPECT_EQ(*instance.parameters.albedoReference, *duplicate.group.texture);
    }
    host.Shutdown();
}

TEST(RigWorkflowIntegration, TestBot) {
    RunWorkflow(fs::path(SRI_SOURCE_ROOT) / "example_game/models/TestBot/model.glb");
}

TEST(RigWorkflowIntegration, OptionalExternalGlb) {
#ifdef _WIN32
    const wchar_t* value = _wgetenv(L"SRI_TEST_RIG_SOURCE");
#else
    const char* value = std::getenv("SRI_TEST_RIG_SOURCE");
#endif
    if (!value || !*value) GTEST_SKIP() << "SRI_TEST_RIG_SOURCE is not set";
    RunWorkflow(fs::path(value));
}
} // namespace
