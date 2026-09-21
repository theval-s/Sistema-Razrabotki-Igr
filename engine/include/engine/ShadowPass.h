#pragma once
#include "RenderPass.hpp"

namespace engine {


CascadeInfo GetCascadeData(Vector3 lightDirection, Matrix cameraView, float fov, float aspect, float nearPlane,
                           float farPlane);

inline CascadeInfo GetCascadeData(Vector3 lightDirection, const FPSCamera& camera) {
    return GetCascadeData(lightDirection, camera.GetViewMatrix(), camera.fov, camera.aspectRatio, camera.nearPlane,
                          camera.farPlane);
}

struct ShadowCSMPass : RenderPass {
    static constexpr uint32_t mapSize = 1024;
    ShaderHandle vertexShader, geometryShader;

    void Initialize(RenderingResourceManager& resourceManager, RenderingContext&, RenderingResources& resources) override {
        vertexShader = resourceManager.CompileShader(L"ShadowCSMPass.hlsl", ShaderType::Vertex);
        geometryShader = resourceManager.CompileShader(L"ShadowCSMPass.hlsl", ShaderType::Geometry);
        const TextureDesc shadowTextureDesc{
            .width = mapSize, .height = mapSize, .arraySize = 4,
            .format = DXGI_FORMAT_R24G8_TYPELESS,
            .usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource,
            .srvFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
            .dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT,
        };
        resources.shadowTexture = resourceManager.CreateTexture(shadowTextureDesc);
    }

    void Render(RenderWorld& world, RenderingContext& context, RenderingResources& resources) override {
        context.SetViewport(mapSize, mapSize);
        context.SetRasterizerState(RasterizerState::CullBackWithBias);
        context.ClearDepthView(resources.shadowTexture, 1.0f, std::nullopt);
        context.SetRenderTargets({TextureHandle{}}, resources.shadowTexture);
        context.SetShader(vertexShader);
        context.SetShader(geometryShader);
        context.SetDepthState(DepthState::ReadWrite);

        world.BindDirectionalLightView(context, resources);
        resources.cascadeInfo = GetCascadeData(world.directionalLight.direction, world.camera);
        resources.shadowCascadesCBuffer.Update(context, [&](CascadeBufferData & data){
            data = resources.cascadeInfo.bufferData;
        });
        
        for (const auto & ri : world.renderItems) {
            resources.materialCBuffer.Update(context, [&](MaterialBufferData & data){
                data.specularColor = ri.material.specularColor;
                data.shininess = ri.material.shininess;
                data.specialType = ri.material.specialType;
            });
            
            resources.objectCBuffer.Update(context, [&](ObjectBufferData & data){
                data.worldMatrix = ri.worldMatrix;
                data.normalMatrix = ri.worldMatrix.Invert().Transpose();
            });
            
            context.DrawMesh(ri.mesh);
        }
        
    }
};


inline std::vector<Vector4> GetFrustumCorners(const Matrix& viewMatrix, const Matrix& projectionMatrix) {
    std::vector<Vector4> frustumCorners;
    const auto inv = (viewMatrix * projectionMatrix).Invert();
    for (int x = 0; x < 2; x++) {
        for (int y = 0; y < 2; y++) {
            for (int z = 0; z < 2; z++) {
                Vector4 corner{
                    2.f * static_cast<float>(x) - 1,
                    2.f * static_cast<float>(y) - 1,
                    static_cast<float>(z),
                    1.0f
                };
                auto transformed = Vector4::Transform(corner, inv);
                frustumCorners.push_back(transformed / transformed.w);
            }
        }
    }
    return frustumCorners;
}

inline std::pair<Vector4, Vector4> GetMinMaxVector(const std::vector<Vector4>& frustumCorners) {
    float maxF = 1e30f;
    Vector4 minV{maxF, maxF, maxF, maxF};
    Vector4 maxV = -minV;

    for (auto& corner : frustumCorners) {
        minV = Vector4::Min(minV, corner);
        maxV = Vector4::Max(maxV, corner);
    }
    return {minV, maxV};
}


inline CascadeInfo GetCascadeData(Vector3 lightDirection, Matrix cameraView, float fov, float aspect, float nearPlane,
                           float farPlane) {
    std::array<float, 5> distances{nearPlane, 0, 0, 0, farPlane};
    for (int i = 4; i >= 1; i--) {
        float t = 0.8f;
        float exp = nearPlane * powf(farPlane / nearPlane, static_cast<float>(i) / 4.f);
        float linear = nearPlane + (farPlane - nearPlane) * static_cast<float>(i) / 4.f;
        distances[i] = exp * t + linear * (1.0f - t);
    }
    CascadeInfo result{};
    for (int i = 0; i < 4; i++) {
        float curNear = distances[i];
        float curFar = distances[i + 1];

        result.bufferData.distances[i] = curFar;
        auto proj = Matrix::CreatePerspectiveFieldOfView(fov, aspect, curNear, curFar);
        auto frustumCorners = GetFrustumCorners(cameraView, proj);

        auto center = Vector4::Zero;
        for (auto& corner : frustumCorners) {
            center += corner;
        }
        center /= 8.f;

        float radius = 0.0f;
        for (auto& corner : frustumCorners) {
            radius = (std::max)(radius, (corner - center).Length());
        }
        radius = std::ceil(radius * 16.0f) / 16.0f;

        auto L = lightDirection;
        L.Normalize();
        Vector3 cascadeCenter{center.x, center.y, center.z};

        Vector3 up = Vector3::Up;
        if (std::abs(L.Dot(up)) > 0.99f) {
            up = Vector3::Right;
        }

        //currently light view points at fixed point
        // auto lightView = Matrix::CreateLookAt(-L * 10, Vector3::Zero, Vector3::Up);
        float lightDistance = radius * 2.0f;
        auto lightView = Matrix::CreateLookAt(cascadeCenter - L * lightDistance, cascadeCenter, up);

        for (auto& corner : frustumCorners) {
            corner = Vector4::Transform(corner, lightView);
        }
        auto centerLight = Vector4::Transform(center, lightView);
        auto [minV, maxV] = GetMinMaxVector(frustumCorners);

        constexpr float shadowMapSize = static_cast<float>(512); //translucency map will be 512
        //As this is only for snapping to avoid jitter, 512 would be fine for 1024 size maps too.
        float diameter = radius * 2.0f;
        float texelSize = diameter / shadowMapSize;
        centerLight.x = std::round(centerLight.x / texelSize) * texelSize;
        centerLight.y = std::round(centerLight.y / texelSize) * texelSize;

        minV.x = centerLight.x - radius;
        maxV.x = centerLight.x + radius;
        minV.y = centerLight.y - radius;
        maxV.y = centerLight.y + radius;

        constexpr float zMult = 10.f; //todo: too big, wastes a lot of precision
        minV.z = (minV.z < 0) ? minV.z * zMult : minV.z / zMult;
        maxV.z = (maxV.z < 0) ? maxV.z / zMult : maxV.z * zMult;
        auto lightProj = Matrix::CreateOrthographicOffCenter(minV.x, maxV.x, minV.y, maxV.y, -maxV.z, -minV.z);
        result.viewMatrices[i] = lightView;
        result.projMatrices[i] = lightProj;
        result.bufferData.viewProjections[i] = lightView * lightProj;
    }
    return result;
}

}
