#pragma once

#include <memory>
#include <string>

namespace Rml { class RenderInterface; }
namespace engine {
struct RenderingContext;
std::unique_ptr<Rml::RenderInterface> CreateUiDx11Renderer(RenderingContext&, std::string& error);
void UiDx11BeginFrame(Rml::RenderInterface&, RenderingContext&, int width, int height);
void UiDx11EndFrame(Rml::RenderInterface&, RenderingContext&);
void UiDx11ReleaseSurface(Rml::RenderInterface&) noexcept;
}
