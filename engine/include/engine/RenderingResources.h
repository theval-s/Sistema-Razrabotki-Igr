#pragma once
#include <SimpleMath.h>

#include "Buffer.hpp"
#include "RenderingResourceManager.hpp"
#include "Texture.hpp"

namespace engine {

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

struct MaterialBufferData {
    Vector3 specularColor;
    float shininess;
    uint32_t specialType;
    Vector3 padding;
};

struct ObjectBufferData {
    Matrix worldMatrix;  
    Matrix normalMatrix; //todo: may be split to separate object as only GBuffer and CS passes currently need this
};

struct CascadeBufferData {
    std::array<Matrix, 4> viewProjections{};
    std::array<float, 4> distances{};
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


template<typename T>
struct ConstantBuffer {
    
    //Safe update function, calls callback, then uploads new data to GPU
    template<typename F>
    void Update(RenderingContext & context, F f) {
        f(data_);
        context.UpdateCBuffer(handle_, data_);
    }
    
    //No so safe functions for advanced usage:
    T & GetData() {
        return data_;
    }

    [[nodiscard]] BufferHandle GetHandle() const {
        return handle_;
    }

    explicit ConstantBuffer(RenderingResourceManager& resourceManager) {
        handle_ = resourceManager.CreateConstantBuffer<T>();
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
    uint32_t screenWith, screenHeight;
    
    //Frame CBuf is unused for now
    ConstantBuffer<uint32_t> frameCBuffer;
    ConstantBuffer<ViewBufferData> viewCBuffer;
    ConstantBuffer<MaterialBufferData> materialCBuffer;
    ConstantBuffer<ObjectBufferData> objectCBuffer;
    ConstantBuffer<CascadeBufferData> shadowCascadesCBuffer;
    ConstantBuffer<LightBufferData> lightCBuffer;
    
    TextureHandle shadowTexture;
    GBuffer gBuffer;
    
    //I am not yet sure should it be here or not
    CascadeInfo cascadeInfo;
    
    void Initialize(RenderingResourceManager& resourceManager) {
        viewCBuffer = ConstantBuffer<ViewBufferData>{resourceManager};
        materialCBuffer = ConstantBuffer<MaterialBufferData>{resourceManager};
        objectCBuffer = ConstantBuffer<ObjectBufferData>{resourceManager};
        shadowCascadesCBuffer = ConstantBuffer<CascadeBufferData>{resourceManager};
        lightCBuffer = ConstantBuffer<LightBufferData>{resourceManager};
    }
    
    void BindCBuffers(RenderingContext& context) {
        //context.BindCBuffer(BindSlot::CBuffer::Frame, frameCBuffer.GetHandle());
        context.BindCBuffer(BindSlot::CBuffer::View, viewCBuffer.GetHandle());
        context.BindCBuffer(BindSlot::CBuffer::Material, viewCBuffer.GetHandle());
        context.BindCBuffer(BindSlot::CBuffer::Object, viewCBuffer.GetHandle());
        context.BindCBuffer(BindSlot::CBuffer::ShadowCascades, viewCBuffer.GetHandle());
    }
};


}
