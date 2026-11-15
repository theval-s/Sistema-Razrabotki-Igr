#include <engine/scene/SceneDocument.hpp>
#include <engine/scene/Scene.hpp>

#include <gtest/gtest.h>
#include <limits>

TEST(AnimationSceneDocument, RoundTripsMissingAndUnassignedReferences) {
    engine::Scene scene;
    const auto entity = scene.CreateEntity("Animated");
    engine::SceneEntityDefinition definition;
    definition.id = scene.GetId(entity);
    definition.name = "Animated";
    definition.skinnedRenderer = engine::SceneEntityDefinition::Mesh{
        {engine::NewUuid(), "game/missing/character.skinned-mesh.json"}, {engine::NewUuid(), "game/missing/material.json"}};
    definition.animator = engine::Animator{{engine::NewUuid(), "game/missing/rig.skeleton.json"}, {}, 2.0f, false, true};
    engine::SceneDocument document;
    document.entities.push_back(definition);
    std::string text, error;
    ASSERT_TRUE(engine::SerializeSceneDocument(document, text, error)) << error;
    engine::SceneDocument parsed;
    ASSERT_TRUE(engine::ParseSceneDocument(text, parsed, error)) << error;
    ASSERT_EQ(parsed.entities.size(), 1u);
    EXPECT_EQ(parsed.entities[0], definition);
}

TEST(AnimationSceneDocument, RejectsInvalidAnimatorAndRendererCombination) {
    engine::Scene scene;
    engine::SceneEntityDefinition definition;
    definition.id = scene.GetId(scene.CreateEntity("Animated"));
    definition.name = "Animated";
    definition.animator = engine::Animator{};
    engine::SceneDocument document;
    document.entities.push_back(definition);
    std::string text, error;
    document.entities[0].animator->speed = -1;
    EXPECT_FALSE(engine::SerializeSceneDocument(document, text, error));
    document.entities[0].animator->speed = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(engine::SerializeSceneDocument(document, text, error));
    document.entities[0].animator->speed = 1;
    const engine::SceneEntityDefinition::Mesh refs{{engine::NewUuid(), "game/missing/mesh"},
                                                   {engine::NewUuid(), "game/missing/material"}};
    document.entities[0].renderer = refs;
    document.entities[0].skinnedRenderer = refs;
    EXPECT_FALSE(engine::SerializeSceneDocument(document, text, error));
}
