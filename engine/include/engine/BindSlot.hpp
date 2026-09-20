#pragma once
#include "Buffer.hpp"

namespace engine {
struct RenderingContext;

enum class ShaderStage : uint8_t {
    None = 0,
    Vertex = 1 << 0,
    Pixel = 1 << 1,
    Geometry = 1 << 2,
    Hull = 1 << 3,
    Domain = 1 << 4,
    Compute = 1 << 5,
};

DEFINE_ENUM_FLAG_OPERATORS(ShaderStage);

inline boolean HasFlag(const ShaderStage slot, const ShaderStage flag) {
    return (slot & flag) != ShaderStage::None;
}


struct BindSlot {
private:
    constexpr static ShaderStage shaderStageAll = ShaderStage::Vertex | ShaderStage::Pixel | ShaderStage::Geometry |
        ShaderStage::Hull | ShaderStage::Domain | ShaderStage::Compute;

public:
    struct CBuffer {
        static constexpr BindSlot Frame{0, shaderStageAll};
        static constexpr BindSlot View{1, shaderStageAll};
        static constexpr BindSlot Material{2, ShaderStage::Vertex};
        static constexpr BindSlot Object{3, shaderStageAll};
        //static constexpr BindSlot ObjectNormal{4, shaderStageAll};
        static constexpr BindSlot ShadowCascades{4, shaderStageAll};
        
        static constexpr BindSlot Pass0{5, shaderStageAll}; //Light buffers for light
        static constexpr BindSlot Pass1{6, shaderStageAll};
        static constexpr BindSlot Pass2{7, shaderStageAll};
    };

    struct Texture {
        static constexpr BindSlot Albedo{0, ShaderStage::Pixel};
        static constexpr BindSlot Normal{1, ShaderStage::Pixel};
        static constexpr BindSlot Material{2, ShaderStage::Pixel};
        static constexpr BindSlot Depth{3, ShaderStage::Pixel};
        static constexpr BindSlot Shadow{4, ShaderStage::Pixel};

        static constexpr BindSlot Pass0{4, ShaderStage::Pixel};
        static constexpr BindSlot Pass1{5, ShaderStage::Pixel};
        static constexpr BindSlot Pass2{6, ShaderStage::Pixel};
        static constexpr BindSlot Pass3{7, ShaderStage::Pixel};
    };


    friend RenderingContext;

    [[nodiscard]] BindSlot WithStages(const ShaderStage stages) const {
        return BindSlot(id_, stages);
    }
    
    [[nodiscard]] bool HasStage(const ShaderStage stage) const {
        return HasFlag(stages_, stage);
    }

    explicit BindSlot(const uint8_t id, const ShaderStage stages) : id_(id), stages_(stages) {
        
    }
    
private :
    uint8_t id_{};
    ShaderStage stages_{};


};
}
