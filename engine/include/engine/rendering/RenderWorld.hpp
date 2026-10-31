#pragma once
#include <SimpleMath.h>

#include <engine/engine_export.hpp>

#include <engine/rendering/FPSCamera.hpp>
#include <engine/rendering/RenderingContext.hpp>
#include <engine/rendering/RenderingResources.hpp>

#include "resources/Material.hpp"

namespace engine {
using DirectX::SimpleMath::Vector3;
using DirectX::SimpleMath::Matrix;

struct LightInfo {
    Vector3 position;
    Vector3 direction;
    Vector3 color;
    float intensity;
    float range;

    enum LightType : uint32_t { Directional = 0, Point = 1, Spot = 2 } type;

    // Cosines of the authored spot half angles; only used for spot lights.
    float spotInnerAngle = 0.0f;
    float spotOuterAngle = 0.0f;
};

struct RenderItem {
    Matrix worldMatrix;
    MeshHandle mesh;
    MaterialInstanceHandle materialInstance;
};

struct RenderWorld {
    FPSCamera camera;
    std::vector<RenderItem> renderItems;
    // Scene extraction replaces only this list. Host-owned procedural draw
    // items may coexist without being claimed by the active scene asset scope.
    std::vector<RenderItem> sceneItems;
    // Supplied by an editor host; empty in games. Uses the same mesh/material as sceneItems.
    std::vector<RenderItem> selectedSceneItems;
    // Editor-owned world-space line endpoints, drawn over the final scene.
    std::vector<Vector3> editorLines;
    // Rebuilt from scene light components by RenderExtractionSubsystem.
    std::vector<LightInfo> lights;
    LightInfo directionalLight; //as for now only one dir light allowed


    ENGINE_API void BindDirectionalLightView(RenderingContext& context, RenderingResources& resources);

    ENGINE_API void BindCameraView(RenderingContext& context, RenderingResources& resources);
};

}
