#pragma once
#include <array>
#include <SimpleMath.h>

#include <engine/engine_export.hpp>

#include <engine/rendering/resources/Buffer.hpp>
#include <engine/rendering/RenderingContext.hpp>
#include <engine/rendering/RenderingResourceManager.hpp>
#include <engine/rendering/resources/Texture.hpp>

namespace engine {

inline constexpr uint32_t ShadowMapSize = 2048;

using DirectX::SimpleMath::Vector2;
using DirectX::SimpleMath::Vector3;
using DirectX::SimpleMath::Vector4;
using DirectX::SimpleMath::Matrix;

struct ViewBufferData {
    Matrix viewMatrix;
    Matrix projectionMatrix;
    Matrix invViewProj;

    Vector3 cameraPos;
    float padding1_;

    Vector2 viewportSize;
    float padding2_;
    float padding3_;
};

struct FrameBufferData {
    float simulationTimeSeconds = 0.0f;
    float padding[3]{};
};

struct MaterialBufferData {
    Vector3 specularColor;
    float shininess;
};

struct ObjectBufferData {
    Matrix worldMatrix;
    Matrix normalMatrix; //todo: may be split to separate object as only GBuffer and CS passes currently need this
};

struct CascadeBufferData {
    std::array<Matrix, 4> viewProjections{};
    std::array<float, 4> distances{};
    std::array<float, 4> transitionStarts{};
    std::array<float, 4> worldTexelSizes{};
};

struct LightBufferData {
    Vector3 lightColor;
    float intensity;

    //now poor man's union
    uint32_t lightType; // 0 = directional, 1 - point, 2 - spot
    float spotInnerAngle;
    float spotOuterAngle;
    float range;

    Vector3 position;
    float padding0_;
    Vector3 direction;
    float padding1_;
};


template <typename T>
struct ConstantBuffer {

    //Safe update function, calls callback, then uploads new data to GPU
    template <typename F>
    void Update(RenderingContext& context, F f) {
        f(data_);
        context.UpdateCBuffer(handle_, data_);
    }

    //No so safe functions for advanced usage:
    T& GetData() {
        return data_;
    }

    [[nodiscard]] BufferHandle GetHandle() const {
        return handle_;
    }
    
    void Initialize(RenderingResourceManager& resourceManager) {
        handle_ = resourceManager.CreateConstantBuffer<T>();
    }
    
    void Destroy(RenderingResourceManager& resourceManager) {
        resourceManager.DestroyBuffer(handle_);
    }

private:
    T data_{};
    BufferHandle handle_{};
};

struct CascadeInfo {
    CascadeBufferData bufferData{};
    std::array<Matrix, 4> viewMatrices{};
    std::array<Matrix, 4> projMatrices{};
};

struct GBuffer {
    TextureHandle albedoTexture;
    TextureHandle normalTexture;
    TextureHandle materialTexture;
    TextureHandle depthTexture;
    TextureHandle resultTexture;
};

//Struct for handling shared state that persists between passes
struct RenderingResources {
    uint32_t screenWith = 0;
    uint32_t screenHeight = 0;

    ConstantBuffer<FrameBufferData> frameCBuffer;
    ConstantBuffer<ViewBufferData> viewCBuffer;
    ConstantBuffer<MaterialBufferData> materialCBuffer;
    ConstantBuffer<ObjectBufferData> objectCBuffer;
    ConstantBuffer<CascadeBufferData> shadowCascadesCBuffer;
    ConstantBuffer<LightBufferData> lightCBuffer;

    TextureHandle shadowTexture;
    // R = visible selected pixels, G = selected pixels behind scene depth.
    TextureHandle selectionMask;
    GBuffer gBuffer;
    
    TextureHandle defaultAlbedoTexture_{};

    //I am not yet sure should it be here or not
    CascadeInfo cascadeInfo;

    ENGINE_API explicit RenderingResources(uint32_t aScreenWith, uint32_t aScreenHeight);

    ENGINE_API void BindCBuffers(RenderingContext& context);
    
    ENGINE_API void Initialize(RenderingResourceManager& resourceManager);
    ENGINE_API void Destroy(RenderingResourceManager& resourceManager);
};


}
