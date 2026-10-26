#include <engine/rendering/passes/ShadowPass.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <engine/rendering/MaterialDrawList.hpp>

namespace engine {
namespace {

constexpr uint32_t mapSize = ShadowMapSize;

std::vector<Vector4> GetFrustumCorners(const Matrix& viewMatrix, const Matrix& projectionMatrix) {
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

CascadeInfo GetCascadeData(Vector3 lightDirection, Matrix cameraView, float fov, float aspect, float nearPlane,
                           float farPlane) {
    // Stable sphere: constant projection scale under camera rotation, but wastes shadow texels.
    // Tight fit: more detail, but projection scale changes as the camera rotates.
    constexpr bool useStableSphereFit = true;
    lightDirection.Normalize();
    std::array<float, 5> distances{nearPlane, 0, 0, 0, farPlane};
    // Blend log and linear scales.
    for (int i = 4; i >= 1; i--) {
        constexpr float t = 0.9f;
        float exp = nearPlane * powf(farPlane / nearPlane, static_cast<float>(i) / 4.f);
        float linear = nearPlane + (farPlane - nearPlane) * static_cast<float>(i) / 4.f;
        distances[i] = exp * t + linear * (1.0f - t);
    }
    CascadeInfo result{};
    // Each following cascade renders the last part of its predecessor's depth range as well, 
    // so lighting can blend two valid shadow samples at a split.
    constexpr float transitionFraction = 0.01f; //todo: make an engine setting
    for (int i = 0; i < 3; ++i) {
        result.bufferData.transitionStarts[i] =
            distances[i + 1] - (distances[i + 1] - distances[i]) * transitionFraction;
    }
    for (int i = 0; i < 4; i++) {
        float curNear = i == 0 ? distances[i] : result.bufferData.transitionStarts[i - 1];
        float curFar = distances[i + 1];

        result.bufferData.distances[i] = curFar;
        auto proj = Matrix::CreatePerspectiveFieldOfView(fov, aspect, curNear, curFar);
        auto frustumCorners = GetFrustumCorners(cameraView, proj);

        Vector3 up = Vector3::Up;
        if (std::abs(lightDirection.Dot(up)) > 0.99f) {
            up = Vector3::Right;
        }

        // Anchor the light axes in world space so the snapped projection follows a fixed grid.
        const auto lightView = Matrix::CreateLookAt(-lightDirection, Vector3::Zero, up);
        Matrix lightProj;
        if constexpr (useStableSphereFit) {
            Vector4 center = Vector4::Zero;
            for (const auto& corner : frustumCorners) {
                center += corner;
            }
            center /= static_cast<float>(frustumCorners.size());

            // The sphere's radius is independent of camera orientation.
            const float halfDepth = (curFar - curNear) * 0.5f;
            const float farHalfHeight = curFar * std::tan(fov * 0.5f);
            const float farCornerRadius = farHalfHeight * std::sqrt(1.0f + aspect * aspect);
            float radius = std::sqrt(halfDepth * halfDepth + farCornerRadius * farCornerRadius);
            // Leave room for PCF taps and center snapping at the projection edge.
            radius = std::ceil(radius * 16.0f) / 16.0f * 1.01f;

            auto centerLight = Vector4::Transform(center, lightView);
            const float texelSize = 2.0f * radius / static_cast<float>(ShadowMapSize);
            result.bufferData.worldTexelSizes[i] = texelSize;
            centerLight.x = std::round(centerLight.x / texelSize) * texelSize;
            centerLight.y = std::round(centerLight.y / texelSize) * texelSize;

            const float casterMargin = radius * 2.0f;
            const float minZ = centerLight.z - radius - casterMargin;
            const float maxZ = centerLight.z + radius + casterMargin;
            // Reverse-Z: swap the light-space near and far distances.
            lightProj = Matrix::CreateOrthographicOffCenter(
                centerLight.x - radius, centerLight.x + radius,
                centerLight.y - radius, centerLight.y + radius, -minZ, -maxZ);
        } else {
            float minX = std::numeric_limits<float>::max();
            float minY = minX;
            float minZ = minX;
            float maxX = std::numeric_limits<float>::lowest();
            float maxY = maxX;
            float maxZ = maxX;
            for (const auto& corner : frustumCorners) {
                const auto p = Vector4::Transform(corner, lightView);
                minX = std::min(minX, p.x);
                maxX = std::max(maxX, p.x);
                minY = std::min(minY, p.y);
                maxY = std::max(maxY, p.y);
                minZ = std::min(minZ, p.z);
                maxZ = std::max(maxZ, p.z);
            }

            // Make room for filtering and for the half-texel shift when snapping the
            // center. Snap in world-aligned light space to keep translations stable.
            constexpr float borderTexels = 2.0f;
            constexpr float usableTexels = static_cast<float>(ShadowMapSize) - 2.0f * borderTexels;
            const float texelX = (maxX - minX) / usableTexels;
            const float texelY = (maxY - minY) / usableTexels;
            const float centerX = std::round((minX + maxX) * 0.5f / texelX) * texelX;
            const float centerY = std::round((minY + maxY) * 0.5f / texelY) * texelY;
            const float halfWidth = (maxX - minX) * 0.5f + borderTexels * texelX;
            const float halfHeight = (maxY - minY) * 0.5f + borderTexels * texelY;
            result.bufferData.worldTexelSizes[i] =
                2.0f * std::max(halfWidth, halfHeight) / static_cast<float>(ShadowMapSize);

            // Off-frustum geometry may cast onto visible receivers from along the light direction.
            const float casterMargin = std::max({maxX - minX, maxY - minY, maxZ - minZ});
            minZ -= casterMargin;
            maxZ += casterMargin;
            lightProj = Matrix::CreateOrthographicOffCenter(
                centerX - halfWidth, centerX + halfWidth,
                centerY - halfHeight, centerY + halfHeight, -minZ, -maxZ);
        }
        result.viewMatrices[i] = lightView;
        result.projMatrices[i] = lightProj;
        result.bufferData.viewProjections[i] = lightView * lightProj;
    }
    return result;
}

CascadeInfo GetCascadeData(Vector3 lightDirection, const FPSCamera& camera) {
    return GetCascadeData(lightDirection, camera.GetViewMatrix(), camera.fov, camera.aspectRatio, camera.nearPlane,
                          camera.farPlane);
}

}

void ShadowCSMPass::Initialize(RenderingResourceManager& resourceManager, RenderingContext&,
                               RenderingResources& resources) {
    vertexShader_ = resourceManager.CompileShader(L"engine/MaterialTemplate.hlsl", ShaderType::Vertex,
        {.defines = {{"MATERIAL_SHADOW_VS"}}});
    geometryShader_ = resourceManager.CompileShader(L"engine/ShadowCSMPass.hlsl", ShaderType::Geometry);
    const TextureDesc shadowTextureDesc{
        .width = mapSize, .height = mapSize, .arraySize = 4,
        .format = DXGI_FORMAT_R32_TYPELESS,
        .usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource,
        .srvFormat = DXGI_FORMAT_R32_FLOAT,
        .dsvFormat = DXGI_FORMAT_D32_FLOAT,
    };
    resources.shadowTexture = resourceManager.CreateTexture(shadowTextureDesc);
}

void ShadowCSMPass::Destroy(RenderingResourceManager& resourceManager, RenderingContext& context,
                            RenderingResources& resources) {
    resourceManager.DestroyShader(vertexShader_);
    resourceManager.DestroyShader(geometryShader_);
    resourceManager.DestroyTexture(resources.shadowTexture);
}

void ShadowCSMPass::Render(RenderWorld& world, RenderingContext& context, RenderingResources& resources) {
    context.SetViewport(mapSize, mapSize);
    context.SetRasterizerState(RasterizerState::CullBackWithBias);
    context.ClearDepthView(resources.shadowTexture);
    context.SetRenderTargets({TextureHandle{}}, resources.shadowTexture);
    context.SetShader(geometryShader_);
    context.GetDeviceContext()->PSSetShader(nullptr, nullptr, 0);
    context.SetDepthState(DepthState::ReadWrite);

    world.BindDirectionalLightView(context, resources);
    resources.cascadeInfo = GetCascadeData(world.directionalLight.direction, world.camera);
    resources.shadowCascadesCBuffer.Update(context, [&](CascadeBufferData& data){
        data = resources.cascadeInfo.bufferData;
    });

    const auto draws = BuildMaterialDrawList(world, context.GetResourceManager(), true, vertexShader_);
    ShaderHandle boundVS{};
    MaterialInstanceHandle boundInstance{};
    for (const auto& draw : draws) {
        if (draw.vertexShader != boundVS) {
            context.SetShader(draw.vertexShader);
            boundVS = draw.vertexShader;
        }
        if (draw.item->materialInstance != boundInstance) {
            const auto& params = draw.instance->parameters;
            resources.materialCBuffer.Update(context, [&](MaterialBufferData& data){
                data.specularColor = params.parameters.specularColor;
                data.shininess = params.parameters.shininess;
            });
            if (params.customBuffer) context.BindCBuffer(BindSlots::CBuffer::MaterialCustomBuffer, params.customBuffer);
            else context.UnbindCBuffer(BindSlots::CBuffer::MaterialCustomBuffer);
            boundInstance = draw.item->materialInstance;
        }
        const auto& ri = *draw.item;
        resources.objectCBuffer.Update(context, [&](ObjectBufferData& data){
            data.worldMatrix = ri.worldMatrix;
            data.normalMatrix = ri.worldMatrix.Invert().Transpose();
        });

        context.DrawMesh(ri.mesh);
    }
}

}
