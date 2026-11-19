// Included by LuaRuntime.cpp inside namespace engine.
namespace {
    template<class Token> struct LuaRenderHandle {
        Token token;
        PostProcessOwner owner;
        std::uint64_t session;
    };
    using LuaRenderShader = LuaRenderHandle<PostProcessShader>;
    using LuaRenderBuffer = LuaRenderHandle<PostProcessBuffer>;
    using LuaRenderTexture = LuaRenderHandle<PostProcessTexture>;
    using LuaRenderPass = LuaRenderHandle<PostProcessPass>;
    using LuaRenderContext = LuaRenderHandle<PostProcessContext>;

    std::size_t RenderArraySize(const sol::table& table) {
        const auto count = table.size();
        std::size_t keys = 0;
        for (const auto& [key, value] : table) {
            if (key.get_type() != sol::type::number) throw std::invalid_argument("Expected a dense numeric array");
            const double index = key.as<double>();
            if (!std::isfinite(index) || index < 1 || index > count || std::floor(index) != index)
                throw std::invalid_argument("Expected a dense numeric array");
            ++keys;
        }
        if (keys != count) throw std::invalid_argument("Expected a dense numeric array");
        return count;
    }

    ShaderParameterScalar RenderScalar(const sol::object& value) {
        if (value.get_type() == sol::type::boolean) return value.as<bool>();
        if (value.get_type() == sol::type::number) return value.as<double>();
        throw std::invalid_argument("Shader parameter components must be numbers or booleans");
    }

    void RenderShape(const sol::object& value, const std::vector<std::size_t>& shape,
        std::size_t level, std::vector<ShaderParameterScalar>& output) {
        if (level == shape.size()) {
            output.push_back(RenderScalar(value));
            return;
        }
        if (value.get_type() != sol::type::table) throw std::invalid_argument("Incorrect shader parameter shape");
        const auto table = value.as<sol::table>();
        if (RenderArraySize(table) != shape[level]) throw std::invalid_argument("Incorrect shader parameter shape");
        for (std::size_t i = 1; i <= shape[level]; ++i)
            RenderShape(table.raw_get<sol::object>(i), shape, level + 1, output);
    }

    ShaderParameterUpdate RenderParameter(const ShaderParameterField& field, const sol::object& value) {
        ShaderParameterUpdate update{field.name, {}};
        const auto count = static_cast<std::size_t>(field.rows) * field.columns * field.elements;
        std::vector<std::size_t> shape;
        if (field.isArray) shape.push_back(field.elements);
        if (field.matrixStride) { shape.push_back(field.rows); shape.push_back(field.columns); }
        else if (field.rows * field.columns > 1) shape.push_back(field.rows * field.columns);
        if (value.get_type() == sol::type::table) {
            const auto table = value.as<sol::table>();
            const auto size = RenderArraySize(table);
            if (size == count && (size == 0 || table.raw_get<sol::object>(1).get_type() != sol::type::table)) {
                for (std::size_t i = 1; i <= size; ++i) update.values.push_back(RenderScalar(table.raw_get<sol::object>(i)));
                return update;
            }
        }
        RenderShape(value, shape, 0, update.values);
        return update;
    }

    std::vector<ShaderParameterUpdate> RenderParameters(const std::vector<ShaderParameterField>& fields,
        const sol::table& values) {
        std::vector<ShaderParameterUpdate> updates;
        for (const auto& [key, value] : values) {
            if (key.get_type() != sol::type::string) throw std::invalid_argument("Shader parameter names must be strings");
            const auto name = key.as<std::string>();
            const auto found = std::find_if(fields.begin(), fields.end(), [&](const auto& field) { return field.name == name; });
            if (found == fields.end()) throw std::invalid_argument("Unknown shader parameter: " + name);
            updates.push_back(RenderParameter(*found, value));
        }
        return updates;
    }

    std::uint32_t RenderInteger(const sol::object& object, const char* name, bool positive = false) {
        if (object.get_type() != sol::type::number) throw std::invalid_argument(std::string(name) + " must be an integer");
        const double value = object.as<double>();
        if (!std::isfinite(value) || std::floor(value) != value || value < (positive ? 1 : 0) ||
            value > std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument(std::string(name) + " is out of range");
        return static_cast<std::uint32_t>(value);
    }
}

PostProcessOwner LuaRuntime::Impl::RenderOwnerForCall(bool setup) {
    auto* instance = activeUiOwner;
    if (!instance || !instance->started || instance->faulted || instance->destroying ||
        uiSuspended || shuttingDown || !loadingModules.empty() ||
        (instance->desc.owner && !instance->desc.owner->IsValid()))
        throw std::invalid_argument("Render requires a live started script owner (use OnStart); teardown is forbidden");
    if (setup && renderExecuting)
        throw std::invalid_argument("Create postprocess resources and passes outside rendering callbacks");
    auto* backend = RenderBackend();
    if (!backend) throw std::invalid_argument("Render is unavailable: no RenderingSubsystem");
    if (!instance->renderOwner.value) instance->renderOwner = backend->CreateOwner();
    if (!backend->IsOwnerAlive(instance->renderOwner)) throw std::invalid_argument("Render owner is stale");
    return instance->renderOwner;
}

void LuaRuntime::Impl::CheckRenderHandle(PostProcessOwner owner, std::uint64_t handleSession) {
    if (handleSession != session || RenderOwnerForCall() != owner)
        throw std::invalid_argument("Render handle belongs to another owner or stale session");
}

void LuaRuntime::Impl::DispatchRender(std::uint64_t callback, PostProcessContext& context, const PostProcessFrame& frame) {
    if (!state || shuttingDown || uiSuspended) throw std::runtime_error("Render script session has ended");
    const auto found = std::find_if(renderCallbacks.begin(), renderCallbacks.end(),
        [&](const auto& entry) { return entry.id == callback; });
    if (found == renderCallbacks.end()) throw std::runtime_error("Render callback has been revoked");
    Instance* instance = Find(found->instance);
    if (!instance || instance->renderOwner != found->owner ||
        instance->faulted || instance->destroying || !instance->started)
        throw std::runtime_error("Render script owner has ended");
    if (instance->active) throw std::runtime_error("Reentrant Render script receiver");
    // Keep a local reference: owner retirement can erase the callback registry during this call.
    sol::protected_function function = found->function;
    const auto diagnostic = instance->Context() + " [callback=Render.execute, pass=" + found->name + "]";
    Scope scope(*this, instance->desc.owner ? instance->desc.owner->GetScene() : nullptr);
    OwnerScope ownerScope(*this, instance);
    renderExecuting = true;
    struct RenderScope { bool& executing; ~RenderScope() { executing = false; } } renderScope{renderExecuting};
    instance->active = true;
    struct Reset { bool& active; ~Reset() { active = false; } } reset{instance->active};
    try {
        sol::table input = state->create_table();
        const auto texture = [&](PostProcessTexture token) {
            return LuaRenderTexture{token, instance->renderOwner, session};
        };
        input["inputColor"] = texture(frame.inputColor);
        input["outputColor"] = texture(frame.outputColor);
        input["depth"] = texture(frame.depth);
        input["normals"] = texture(frame.normals);
        input["inputWidth"] = frame.inputWidth;
        input["inputHeight"] = frame.inputHeight;
        input["outputWidth"] = frame.outputWidth;
        input["outputHeight"] = frame.outputHeight;
        renderDeviceFailure = {};
        auto result = function(LuaRenderContext{context, instance->renderOwner, session}, input);
        // sol translates native exceptions into Lua errors. Preserve typed GPU failures
        // separately so the renderer's device-failure boundary still sees the original.
        if (renderDeviceFailure) std::rethrow_exception(std::exchange(renderDeviceFailure, {}));
        std::string error;
        if (!CheckResult(result, error, diagnostic)) {
            instance->faulted = true;
            instance->failure = error;
            throw std::runtime_error(error);
        }
    } catch (const RenderingException&) {
        throw;
    } catch (const std::exception& exception) {
        instance->faulted = true;
        if (instance->failure.empty()) {
            instance->failure = diagnostic + ": " + exception.what();
            engine::log::Lua().error("{}", instance->failure);
        }
        throw;
    }
}

void LuaRuntime::Impl::RegisterRender() {
    auto render = state->create_named_table("Render");
    auto stages = state->create_table();
    stages["BeforeTonemap"] = PostProcessStage::BeforeTonemap;
    stages["AfterTonemap"] = PostProcessStage::AfterTonemap;
    stages["AfterUI"] = PostProcessStage::AfterUI;
    render["Stage"] = stages;
    state->new_usertype<LuaRenderShader>("RenderShader", sol::no_constructor);
    state->new_usertype<LuaRenderTexture>("RenderTexture", sol::no_constructor);
    state->new_usertype<LuaRenderBuffer>("RenderConstantBuffer", sol::no_constructor,
        "Set", sol::overload(
            [this](const LuaRenderBuffer& buffer, sol::table values) {
                CheckRenderHandle(buffer.owner, buffer.session);
                auto updates = RenderParameters(RenderBackend()->GetParameterFields(buffer.owner, buffer.token), values);
                RenderBackend()->SetParameters(buffer.owner, buffer.token, updates);
            },
            [this](const LuaRenderBuffer& buffer, const std::string& name, sol::object value) {
                CheckRenderHandle(buffer.owner, buffer.session);
                auto values = state->create_table();
                values[name] = value;
                auto updates = RenderParameters(RenderBackend()->GetParameterFields(buffer.owner, buffer.token), values);
                RenderBackend()->SetParameters(buffer.owner, buffer.token, updates);
            }));
    state->new_usertype<LuaRenderPass>("RenderPass", sol::no_constructor,
        "Remove", [this](const LuaRenderPass& pass) {
            CheckRenderHandle(pass.owner, pass.session);
            RenderBackend()->RemovePass(pass.owner, pass.token);
            std::erase_if(renderCallbacks, [&](const auto& entry) {
                return entry.owner == pass.owner && entry.pass == pass.token;
            });
        },
        "SetEnabled", [this](const LuaRenderPass& pass, sol::object enabled) {
            CheckRenderHandle(pass.owner, pass.session);
            if (enabled.get_type() != sol::type::boolean) throw std::invalid_argument("SetEnabled expects a boolean");
            RenderBackend()->SetEnabled(pass.owner, pass.token, enabled.as<bool>());
        },
        "GetError", [this](const LuaRenderPass& pass) {
            CheckRenderHandle(pass.owner, pass.session);
            return RenderBackend()->GetPassError(pass.owner, pass.token);
        });

    // Only context methods can issue GPU commands during an executing pass.
    const auto invoke = [this](auto&& operation) -> decltype(auto) {
        try { return operation(); }
        catch (const RenderingException&) { renderDeviceFailure = std::current_exception(); throw; }
    };
    state->new_usertype<LuaRenderContext>("RenderContext", sol::no_constructor,
        "SetPixelShader", [this, invoke](LuaRenderContext& context, const LuaRenderShader& shader) {
            CheckRenderHandle(context.owner, context.session); CheckRenderHandle(shader.owner, shader.session);
            invoke([&] { context.token.SetPixelShader(shader.token); });
        },
        "SetRenderTarget", [this, invoke](LuaRenderContext& context, const LuaRenderTexture& texture) {
            CheckRenderHandle(context.owner, context.session); CheckRenderHandle(texture.owner, texture.session);
            invoke([&] { context.token.SetRenderTarget(texture.token); });
        },
        "BindTexture", [this, invoke](LuaRenderContext& context, sol::object slot, const LuaRenderTexture& texture) {
            CheckRenderHandle(context.owner, context.session); CheckRenderHandle(texture.owner, texture.session);
            invoke([&] { context.token.BindTexture(RenderInteger(slot, "Texture slot"), texture.token); });
        },
        "BindConstantBuffer", [this, invoke](LuaRenderContext& context, sol::object slot, const LuaRenderBuffer& buffer) {
            CheckRenderHandle(context.owner, context.session); CheckRenderHandle(buffer.owner, buffer.session);
            const auto index = RenderInteger(slot, "Constant buffer slot");
            if (index < 5) throw std::invalid_argument("Constant buffer slots b0-b4 are reserved");
            invoke([&] { context.token.BindConstantBuffer(index, buffer.token); });
        },
        "ClearRenderTarget", [this, invoke](LuaRenderContext& context, const LuaRenderTexture& texture, sol::optional<sol::table> color) {
            CheckRenderHandle(context.owner, context.session); CheckRenderHandle(texture.owner, texture.session);
            std::array<float, 4> rgba{};
            if (color) {
                if (RenderArraySize(*color) != 4) throw std::invalid_argument("Clear color requires four numbers");
                for (int i = 0; i < 4; ++i) {
                    auto value = color->raw_get<sol::object>(i + 1);
                    if (value.get_type() != sol::type::number || !std::isfinite(value.as<double>()) ||
                        std::abs(value.as<double>()) > std::numeric_limits<float>::max())
                        throw std::invalid_argument("Clear color requires finite numbers");
                    rgba[i] = static_cast<float>(value.as<double>());
                }
            }
            invoke([&] { context.token.ClearRenderTarget(texture.token, rgba); });
        },
        "DrawFullscreen", [this, invoke](LuaRenderContext& context) {
            CheckRenderHandle(context.owner, context.session);
            invoke([&] { context.token.DrawFullscreen(); });
        },
        "GetTextureSize", [this, invoke](LuaRenderContext& context, const LuaRenderTexture& texture) {
            CheckRenderHandle(context.owner, context.session); CheckRenderHandle(texture.owner, texture.session);
            const auto size = invoke([&] { return context.token.GetTextureSize(texture.token); });
            return std::make_tuple(size[0], size[1]);
        });

    render.set_function("SetBuiltinTonemappingEnabled", [this](sol::object enabled) {
        if (enabled.get_type() != sol::type::boolean)
            throw std::invalid_argument("SetBuiltinTonemappingEnabled expects a boolean");
        const auto owner = RenderOwnerForCall(true);
        RenderBackend()->SetBuiltinTonemappingEnabled(owner, enabled.as<bool>());
    });
    render.set_function("LoadShader", [this](const std::string& asset) {
        const auto owner = RenderOwnerForCall(true);
        const auto& catalog = bindingsContext.engine->GetAssets().GetCatalog();
        const auto* entry = catalog.Find(std::string_view(asset), CatalogAssetKind::PostProcess);
        if (!entry) entry = catalog.FindPath(asset);
        if (!entry || entry->metadata.kind != CatalogAssetKind::PostProcess)
            throw std::invalid_argument("Render.LoadShader expects a catalogued postprocess UUID or logical path");
        PostProcessDefinition definition;
        ShaderVariant variant;
        std::string error;
        if (!LoadPostProcessDefinition(entry->source, definition, error) ||
            !PostProcessShaderVariant(definition, catalog, variant, error))
            throw std::invalid_argument(error);
        try {
            const auto compiled = ShaderContext::CompileCpu(variant.file, variant.stage, variant.options);
            return LuaRenderShader{RenderBackend()->CreateShader(owner, compiled), owner, session};
        } catch (const RenderingException& error) {
            throw std::runtime_error(rendering_failure::Format(error.GetFailure()));
        }
    });
    render.set_function("LoadTexture", [this](const std::string& asset) {
        const auto owner = RenderOwnerForCall(true);
        auto& assets = bindingsContext.engine->GetAssets();
        const auto& catalog = assets.GetCatalog();
        const auto* entry = catalog.Find(std::string_view(asset), CatalogAssetKind::Texture);
        if (!entry) entry = catalog.FindPath(asset);
        if (!entry || entry->metadata.kind != CatalogAssetKind::Texture)
            throw std::invalid_argument("Render.LoadTexture expects a catalogued texture UUID or logical path");
        const auto texture = assets.LoadTexture(entry->path);
        if (!texture) throw std::runtime_error("Could not load postprocess texture: " + asset);
        return LuaRenderTexture{RenderBackend()->ImportTexture(owner, texture->texture), owner, session};
    });
    render.set_function("CreateConstantBuffer", [this](sol::table desc) {
        const auto owner = RenderOwnerForCall(true);
        const auto shader = desc.get<LuaRenderShader>("shader");
        CheckRenderHandle(shader.owner, shader.session);
        const auto name = desc.get<std::string>("name");
        const auto layout = RenderBackend()->ReflectParameters(owner, shader.token, name);
        std::vector<ShaderParameterUpdate> updates;
        auto values = desc.raw_get<sol::object>("values");
        if (values.get_type() != sol::type::nil) {
            if (values.get_type() != sol::type::table) throw std::invalid_argument("Constant buffer values must be a table");
            updates = RenderParameters(layout.Fields(), values.as<sol::table>());
        }
        const auto token = RenderBackend()->CreateConstantBuffer(owner, shader.token, name, updates);
        return LuaRenderBuffer{token, owner, session};
    });
    render.set_function("CreateRenderTarget", [this](sol::table desc) {
        const auto owner = RenderOwnerForCall(true);
        PostProcessTargetDesc target;
        const auto width = desc.raw_get<sol::object>("width");
        const auto height = desc.raw_get<sol::object>("height");
        const auto scale = desc.raw_get<sol::object>("scale");
        if (width.get_type() != sol::type::nil || height.get_type() != sol::type::nil) {
            if (scale.get_type() != sol::type::nil) throw std::invalid_argument("Use width/height or scale, not both");
            target.width = RenderInteger(width, "Target width", true);
            target.height = RenderInteger(height, "Target height", true);
        } else if (scale.get_type() != sol::type::nil) {
            if (scale.get_type() != sol::type::number || !std::isfinite(scale.as<double>()) ||
                scale.as<double>() <= 0 || scale.as<double>() > std::numeric_limits<float>::max())
                throw std::invalid_argument("Target scale must be positive and finite");
            target.scale = static_cast<float>(scale.as<double>());
        }
        const auto format = desc.get_or<std::string>("format", "RGBA16F");
        if (format == "RGBA16F") target.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        else if (format == "RGBA8") target.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        else if (format == "R16F") target.format = DXGI_FORMAT_R16_FLOAT;
        else if (format == "R8") target.format = DXGI_FORMAT_R8_UNORM;
        else throw std::invalid_argument("Unsupported render target format");
        return LuaRenderTexture{RenderBackend()->CreateTarget(owner, target), owner, session};
    });
    render.set_function("AddFullscreenPass", [this](sol::table desc) {
        const auto owner = RenderOwnerForCall(true);
        const auto name = desc.get<std::string>("name");
        const auto stage = desc.get<PostProcessStage>("stage");
        if (stage != PostProcessStage::BeforeTonemap && stage != PostProcessStage::AfterTonemap &&
            stage != PostProcessStage::AfterUI) throw std::invalid_argument("Invalid Render stage");
        int order = 0;
        const auto orderValue = desc.raw_get<sol::object>("order");
        if (orderValue.get_type() != sol::type::nil) {
            if (orderValue.get_type() != sol::type::number) throw std::invalid_argument("Pass order must be an integer");
            const auto number = orderValue.as<double>();
            if (!std::isfinite(number) || std::floor(number) != number || number < std::numeric_limits<int>::min() ||
                number > std::numeric_limits<int>::max()) throw std::invalid_argument("Pass order is out of range");
            order = static_cast<int>(number);
        }
        const auto function = desc.get<sol::protected_function>("execute");
        if (!function.valid()) throw std::invalid_argument("Pass execute must be a function");
        if (nextRenderCallback == std::numeric_limits<std::uint64_t>::max())
            throw std::runtime_error("Render callback IDs exhausted");
        const auto callback = nextRenderCallback++;
        InstanceId instance;
        for (const auto handle : instances.Handles()) {
            if (instances.Get(handle).get() == activeUiOwner) { instance = Id(handle); break; }
        }
        if (!instance.IsValid()) throw std::invalid_argument("Render owner is not registered in this script session");
        renderCallbacks.push_back({callback, owner, instance, function, {}, name});
        try {
            const std::weak_ptr<UiLink> weak = uiLink;
            const auto pass = RenderBackend()->AddPass(owner, name, stage, order,
                [weak, callback](PostProcessContext& context, const PostProcessFrame& frame) {
                    const auto link = weak.lock();
                    if (!link || !link->runtime) throw std::runtime_error("Render script runtime has ended");
                    link->runtime->DispatchRender(callback, context, frame);
                });
            const auto record = std::find_if(renderCallbacks.begin(), renderCallbacks.end(),
                [&](const auto& entry) { return entry.id == callback; });
            if (record != renderCallbacks.end()) record->pass = pass;
            return LuaRenderPass{pass, owner, session};
        } catch (...) {
            std::erase_if(renderCallbacks, [&](const auto& entry) { return entry.id == callback; });
            throw;
        }
    });
}
