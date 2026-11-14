#include <editor/scene_events.hpp>
#include <editor/scene_history.hpp>
#include <editor/selection.hpp>

#include <engine/engine.hpp>
#include <engine/audio/AudioComponents.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/scene/Scene.hpp>

#include <gtest/gtest.h>

namespace {

using DirectX::SimpleMath::Vector3;

class SceneHistoryTest : public testing::Test {
protected:
    engine::Scene& scene() { return host.GetScene(); }

    void moveTo(const entt::entity entity, const float x) {
        engine::Transform transform = scene().GetTransform(entity);
        transform.position.x = x;
        scene().SetTransform(entity, transform);
    }

    engine::Engine host;
    editor::SceneEvents events{host.GetScene()};
    editor::Selection selection{host.GetScene(), events};
    editor::SceneHistory history{host, selection, events};
};

TEST_F(SceneHistoryTest, AnEditUndoesAndRedoesInPlace) {
    const auto box = scene().CreateEntity("Box");
    history.setClean();
    history.edit("Move", [&] { moveTo(box, 3.0f); });
    ASSERT_EQ(history.stack().count(), 1);
    EXPECT_EQ(history.stack().undoText(), "Move");
    EXPECT_FALSE(history.isClean());

    history.stack().undo();
    EXPECT_TRUE(scene().IsValid(box));  // same handle
    EXPECT_EQ(scene().GetTransform(box).position.x, 0.0f);
    EXPECT_TRUE(history.isClean());
    history.stack().redo();
    EXPECT_EQ(scene().GetTransform(box).position.x, 3.0f);
}

TEST_F(SceneHistoryTest, EditsThatChangeNothingAddNoStep) {
    const auto box = scene().CreateEntity("Box");
    history.edit("Nothing", [] {});
    history.edit("Same place", [&] { moveTo(box, 0.0f); });
    EXPECT_EQ(history.stack().count(), 0);
}

TEST_F(SceneHistoryTest, UndoingADeleteBringsBackTheSubtreeAndSelection) {
    const auto lamp = scene().CreateEntity("Lamp");
    scene().AddComponent<engine::PointLight>(lamp, engine::PointLight{{1, 0, 0}, 2.0f, 7.0f});
    const auto bulb = scene().CreateEntity("Bulb", lamp);
    const auto lampId = scene().GetId(lamp);
    const auto bulbId = scene().GetId(bulb);
    selection.select(lamp);

    history.edit("Delete", [&] {
        scene().DestroyEntity(lamp);
        selection.clear();
    });
    ASSERT_TRUE(scene().FindByUuid(lampId) == entt::null);

    history.stack().undo();
    const auto restored = scene().FindByUuid(lampId);
    ASSERT_TRUE(restored != entt::null);
    EXPECT_EQ(scene().GetParent(scene().FindByUuid(bulbId)), restored);
    ASSERT_NE(scene().TryGetComponent<engine::PointLight>(restored), nullptr);
    EXPECT_EQ(scene().TryGetComponent<engine::PointLight>(restored)->range, 7.0f);
    EXPECT_EQ(selection.primary(), restored);

    history.stack().redo();
    EXPECT_TRUE(scene().FindByUuid(lampId) == entt::null);
    EXPECT_TRUE(scene().FindByUuid(bulbId) == entt::null);
}

TEST_F(SceneHistoryTest, UndoingAReparentRestoresTheTransformToo) {
    const auto parent = scene().CreateEntity("Parent");
    moveTo(parent, 10.0f);
    const auto child = scene().CreateEntity("Child");
    moveTo(child, 1.0f);

    history.edit("Reparent", [&] { scene().SetParentKeepingWorld(child, parent); });
    EXPECT_EQ(scene().GetTransform(child).position.x, -9.0f);
    history.stack().undo();
    EXPECT_TRUE(scene().GetParent(child) == entt::null);
    EXPECT_EQ(scene().GetTransform(child).position.x, 1.0f);
}

TEST_F(SceneHistoryTest, EditsWithTheSameKeyMergeUntilSealed) {
    const auto box = scene().CreateEntity("Box");
    for (const float x : {1.0f, 2.0f, 3.0f}) history.edit("Move", [&] { moveTo(box, x); }, "box/x");
    EXPECT_EQ(history.stack().count(), 1);
    history.seal();
    history.edit("Move", [&] { moveTo(box, 4.0f); }, "box/x");
    EXPECT_EQ(history.stack().count(), 2);

    history.stack().undo();
    EXPECT_EQ(scene().GetTransform(box).position.x, 3.0f);
    history.stack().undo();
    EXPECT_EQ(scene().GetTransform(box).position.x, 0.0f);

    // Merged back to where it started: the step goes away.
    history.stack().redo();
    history.edit("Move", [&] { moveTo(box, 0.0f); }, "box/x");  // the redo sealed the step
    EXPECT_EQ(history.stack().count(), 2);
    history.edit("Move", [&] { moveTo(box, 5.0f); }, "box/y");
    history.edit("Move", [&] { moveTo(box, 0.0f); }, "box/y");
    EXPECT_EQ(history.stack().count(), 2);
}

TEST_F(SceneHistoryTest, ATransactionIsOneStepAndCancelPutsItBack) {
    const auto box = scene().CreateEntity("Box");
    history.begin("Drag");
    history.begin("Nested");
    for (int i = 1; i <= 50; ++i) moveTo(box, static_cast<float>(i));
    history.end();
    EXPECT_EQ(history.stack().count(), 0);  // still inside the outer one
    history.end();
    EXPECT_EQ(history.stack().count(), 1);
    EXPECT_EQ(history.stack().undoText(), "Drag");

    history.begin("Drag");
    moveTo(box, 99.0f);
    history.cancel();
    EXPECT_EQ(scene().GetTransform(box).position.x, 50.0f);
    EXPECT_EQ(history.stack().count(), 1);
}

TEST_F(SceneHistoryTest, ChangesOutsideAnEditAreDirtyAndPlayDoesNotCount) {
    history.setClean();
    const auto box = scene().CreateEntity("Unrecorded");
    EXPECT_FALSE(history.isClean());
    history.setClean();

    history.setEnabled(false);
    moveTo(box, 7.0f);
    scene().CreateEntity("Runtime spawn");
    history.setEnabled(true);
    EXPECT_TRUE(history.isClean());

    // The history still applies after Play rebuilt the scene with the same UUIDs.
    history.edit("Rename", [&] { scene().SetName(box, "Renamed"); });
    history.stack().undo();
    EXPECT_EQ(scene().GetName(box), "Unrecorded");
}

TEST_F(SceneHistoryTest, SceneSettingsAreUndone) {
    const auto sun = scene().CreateEntity("Sun");
    scene().AddComponent<engine::DirectionalLight>(sun);
    history.edit("Set sun", [&] { scene().SetDirectionalLight(sun); });
    history.edit("Folders", [&] { scene().SetEditorFolders({"Props"}); });
    history.stack().undo();
    EXPECT_TRUE(scene().GetEditorFolders().empty());
    history.edit("Delete", [&] { scene().DestroyEntity(sun); });
    history.stack().undo();
    EXPECT_EQ(scene().GetDirectionalLight(), scene().FindFirstByName("Sun"));
}

TEST_F(SceneHistoryTest, AudioAuthoringSupportsAddEditRemoveAndMergedUndo) {
    const auto emitter = scene().CreateEntity("Emitter");
    history.edit("Add Audio Source", [&] { scene().AddComponent<engine::AudioSource>(emitter); });
    ASSERT_NE(scene().TryGetComponent<engine::AudioSource>(emitter), nullptr);
    const auto defaults = *scene().TryGetComponent<engine::AudioSource>(emitter);
    history.stack().undo();
    EXPECT_FALSE(scene().HasComponent<engine::AudioSource>(emitter));
    history.stack().redo();

    auto authored = defaults;
    // Missing catalog references must survive history just like resolved clips.
    authored.clip = engine::AssetReference{engine::AssetUuid("ea361a70-8336-47cb-af8e-c228e7d6c54b"), "audio/missing.wav"};
    authored.enabled = false;
    authored.playOnStart = true;
    authored.looping = true;
    authored.loading = engine::AudioLoadingMode::Streamed;
    authored.bus = engine::AudioBus::Music;
    authored.volume = 0.25f;
    authored.pitch = 1.5f;
    authored.spatial = engine::AudioSpatialMode::TwoD;
    authored.attenuation = engine::AudioAttenuation::Linear;
    authored.minDistance = 2.0f;
    authored.maxDistance = 50.0f;
    authored.rolloff = 0.5f;
    history.edit("Change Audio Source", [&] {
        scene().Modify<engine::AudioSource>(emitter, [&](auto& source) { source = authored; });
    }, "audio/source");
    authored.volume = 0.75f;
    history.edit("Change Audio Source", [&] {
        scene().Modify<engine::AudioSource>(emitter, [&](auto& source) { source.volume = authored.volume; });
    }, "audio/source");
    ASSERT_EQ(history.stack().count(), 2);
    history.stack().undo();
    ASSERT_NE(scene().TryGetComponent<engine::AudioSource>(emitter), nullptr);
    EXPECT_EQ(*scene().TryGetComponent<engine::AudioSource>(emitter), defaults);
    history.stack().redo();
    EXPECT_EQ(*scene().TryGetComponent<engine::AudioSource>(emitter), authored);
    history.edit("Remove Audio Source", [&] { scene().RemoveComponent<engine::AudioSource>(emitter); });
    EXPECT_FALSE(scene().HasComponent<engine::AudioSource>(emitter));
    history.stack().undo();
    ASSERT_NE(scene().TryGetComponent<engine::AudioSource>(emitter), nullptr);
    EXPECT_EQ(*scene().TryGetComponent<engine::AudioSource>(emitter), authored);
}

TEST_F(SceneHistoryTest, AudioListenerAndSourceSurviveDeleteUndo) {
    const auto emitter = scene().CreateEntity("Emitter");
    const auto id = scene().GetId(emitter);
    scene().AddComponent<engine::AudioSource>(emitter);
    history.edit("Add Audio Listener", [&] { scene().AddComponent<engine::AudioListener>(emitter); });
    history.stack().undo();
    EXPECT_FALSE(scene().HasComponent<engine::AudioListener>(emitter));
    history.stack().redo();
    history.edit("Disable Listener", [&] {
        scene().Modify<engine::AudioListener>(emitter, [](auto& listener) { listener.enabled = false; });
    });
    history.stack().undo();
    ASSERT_NE(scene().TryGetComponent<engine::AudioListener>(emitter), nullptr);
    EXPECT_TRUE(scene().TryGetComponent<engine::AudioListener>(emitter)->enabled);
    history.stack().redo();
    history.edit("Delete", [&] { scene().DestroyEntity(emitter); });
    history.stack().undo();
    const auto restored = scene().FindByUuid(id);
    ASSERT_TRUE(restored != entt::null);
    ASSERT_NE(scene().TryGetComponent<engine::AudioListener>(restored), nullptr);
    EXPECT_FALSE(scene().TryGetComponent<engine::AudioListener>(restored)->enabled);
    EXPECT_TRUE(scene().HasComponent<engine::AudioSource>(restored));
    history.stack().redo();
    EXPECT_TRUE(scene().FindByUuid(id) == entt::null);
}

}  // namespace
