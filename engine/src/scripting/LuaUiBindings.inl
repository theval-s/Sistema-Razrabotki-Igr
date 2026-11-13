// Included only by LuaRuntime.cpp, inside namespace engine, after the private Impl.
// Keep the real table as both Autoloads.Name and legacy self. Its backing fields
// are private to this Lua closure; every way of obtaining a method returns the
// same guarded trampoline, including rawget/next/pairs and later assignments.
void LuaRuntime::Impl::RegisterAutoloadDispatch() {
    sol::load_result loaded = state->load(R"(
        local rawget0, rawset0, next0, rawlen0 = rawget, rawset, next, rawlen
        local type0, tostring0, error0 = type, tostring, error
        local pack, unpack = table.pack, table.unpack
        local setmetatable0 = setmetatable
        local managed = setmetatable0({}, {__mode = 'k'})
        rawget = function(t, k)
            local m = managed[t]
            if m then return m.expose(k, rawget0(m.data, k)) end
            return rawget0(t, k)
        end
        rawset = function(t, k, v)
            local m = managed[t]
            if m then m.invalidate(k); rawset0(m.data, k, v); return t end
            return rawset0(t, k, v)
        end
        next = function(t, k)
            local m = managed[t]
            if not m then return next0(t, k) end
            local key, value = next0(m.data, k)
            if key ~= nil then return key, m.expose(key, value) end
        end
        rawlen = function(t)
            local m = managed[t]
            return rawlen0(m and m.data or t)
        end
        local function install(target, old, invoke, environment)
            local data, meta, cache = {}, {}, {}
            local wrappers = setmetatable0({}, {__mode = 'k'})
            if old then for k, v in next0, old do meta[k] = v end end
            for k, v in next0, target do data[k] = v end
            for k in next0, data do rawset0(target, k, nil) end
            local function expose(key, value)
                if type0(value) ~= 'function' then return value end
                if wrappers[value] then return value end
                local entry = cache[key]
                if entry and entry.original == value then return entry.wrapper end
                local wrapper = function(...)
                    local result = pack(invoke(tostring0(key), value, ...))
                    if not result[1] then error0(result[2], 0) end
                    return unpack(result, 2, result.n)
                end
                cache[key] = {original = value, wrapper = wrapper}
                wrappers[wrapper] = value
                return wrapper
            end
            local index, newindex = old and old.__index, old and old.__newindex
            meta.__index = function(t, k)
                local value = rawget0(data, k)
                if value ~= nil then return expose(k, value) end
                if type0(index) == 'function' then value = index(t, k)
                elseif index ~= nil then value = index[k] end
                -- Environment fallback is the shared globals (assert, require,
                -- etc.), not methods belonging to this script receiver.
                if environment then return value end
                return expose(k, value)
            end
            meta.__newindex = function(t, k, v)
                cache[k] = nil
                if rawget0(data, k) ~= nil or newindex == nil then rawset0(data, k, v)
                elseif type0(newindex) == 'function' then newindex(t, k, v)
                else newindex[k] = v end
            end
            local pairs0 = old and old.__pairs
            meta.__pairs = function(t)
                if pairs0 then
                    local iterator, context, first = pairs0(t)
                    return function(s, k)
                        local key, value = iterator(s, k)
                        if key ~= nil then return key, expose(key, value) end
                    end, context, first
                end
                return function(_, k)
                    local key, value = next0(data, k)
                    if key ~= nil then return key, expose(key, value) end
                end, t, nil
            end
            if not meta.__len then meta.__len = function() return #data end end
            local function nativeMember(k, ownOnly)
                local value = rawget0(data, k)
                if value == nil and not ownOnly then
                    if type0(index) == 'function' then value = index(target, k)
                    elseif index ~= nil then value = index[k] end
                end
                -- Only unwrap this receiver's trampolines. A method deliberately
                -- borrowed from another autoload must keep that owner's guard.
                while type0(value) == 'function' and wrappers[value] do value = wrappers[value] end
                return value
            end
            managed[target] = {data = data, expose = expose, nativeMember = nativeMember,
                invalidate = function(k) cache[k] = nil end}
            return meta
        end
        -- Native StateValue conversion intentionally uses raw C API iteration,
        -- not __pairs. Snapshot backing fields first so passing a managed table
        -- (also nested in another table) cannot silently serialize as empty.
        local function snapshot(value, seen, depth)
            if type0(value) ~= 'table' or depth > 64 then return value end
            if seen[value] then return seen[value] end
            local copy = {}
            seen[value] = copy
            local entry = managed[value]
            local source = entry and entry.data or value
            for k, v in next0, source do copy[k] = snapshot(v, seen, depth + 1) end
            return copy
        end
        local function lookup(target, key, ownOnly)
            local entry = managed[target]
            if entry then return entry.nativeMember(key, ownOnly) end
            if ownOnly then return rawget0(target, key) end
            return target[key]
        end
        return install, function(value) return snapshot(value, {}, 0) end, lookup
    )", "engine autoload dispatch");
    ThrowIfFailed(loaded);
    sol::protected_function factory = loaded;
    auto result = factory();
    ThrowIfFailed(result);
    installAutoloadFacade = result.get<sol::protected_function>();
    snapshotAutoloadValues = result.get<sol::protected_function>(1);
    lookupAutoloadMember = result.get<sol::protected_function>(2);
}

sol::object LuaRuntime::Impl::ScriptMember(const Instance& instance, const std::string& name) {
    // Native callers have already entered the receiver's Scope/OwnerScope/active
    // guard. Public Lua lookup must remain wrapped, but native dispatch must not
    // add a second protected call and fault/log boundary for the same receiver.
    if (instance.desc.autoloadName.empty())
        return instance.environmentCallbacks ? instance.table.raw_get<sol::object>(name)
            : instance.table.get<sol::object>(name);
    auto result = lookupAutoloadMember(instance.table, name, instance.environmentCallbacks);
    ThrowIfFailed(result);
    return result.get<sol::object>();
}

sol::object LuaRuntime::Impl::StateValueInput(const sol::object& value) {
    auto result = snapshotAutoloadValues(value);
    ThrowIfFailed(result);
    return result.get<sol::object>();
}

void LuaRuntime::Impl::InstallAutoloadDispatch(InstanceId id) {
    auto* instance = Find(id);
    std::weak_ptr<UiLink> weak = uiLink; // Session-revoked link also works in headless runtimes.
    auto invoke = sol::make_object(*state, [weak, id](const std::string& method,
        sol::protected_function function, sol::variadic_args args, sol::this_state luaState) {
        if (auto link = weak.lock(); link && link->runtime)
            return link->runtime->DispatchAutoload(id, method, std::move(function), args, luaState);
        sol::state_view lua(luaState);
        sol::variadic_results result;
        result.push_back(sol::make_object(lua, false));
        result.push_back(sol::make_object(lua, "Expired autoload session"));
        return result;
    });
    // C API metatable access deliberately bypasses __metatable protection, without
    // publishing debug/getregistry or a state accessor to scripts.
    sol::object old = instance->table[sol::metatable_key];
    auto result = installAutoloadFacade(instance->table, old, invoke, instance->environmentCallbacks);
    ThrowIfFailed(result);
    instance->table[sol::metatable_key] = result.get<sol::table>();
}

sol::variadic_results LuaRuntime::Impl::DispatchAutoload(InstanceId id, const std::string& method,
    sol::protected_function function, sol::variadic_args args, sol::this_state luaState) {
    sol::state_view lua(luaState);
    const auto failure = [&](const std::string& error) {
        sol::variadic_results result;
        result.push_back(sol::make_object(lua, false));
        result.push_back(sol::make_object(lua, error));
        return result;
    };
    auto* instance = state && id.session_ == session ? Find(id) : nullptr;
    if (!instance || !instance->started || instance->faulted)
        return failure("Invalid, unstarted, expired or faulted autoload receiver: " + method);
    const bool internal = activeUiOwner == instance;
    if ((instance->active || instance->destroying) && !internal)
        return failure("Reentrant/destroying autoload receiver: " + method);
    // Other surviving services remain callable from OnDestroy. A deferred state
    // reset is never completed here: local function/results still reference Lua.
    Scope scope(*this);
    OwnerScope ownerScope(*this, instance);
    const bool wasActive = instance->active;
    instance->active = true;
    struct Restore { bool& flag; bool value; ~Restore() { flag = value; } } restore{instance->active, wasActive};
    try {
        auto result = function(args); // Preserve exact dot/colon arguments and nil return slots.
        if (!result.valid() && instance->faulted) return failure(instance->failure);
        std::string error;
        if (!CheckResult(result, error, instance->Context() + " [method=" + method + "]")) {
            instance->faulted = true;
            instance->failure = error;
            return failure(error);
        }
        sol::variadic_results values;
        values.push_back(sol::make_object(lua, true));
        for (int i = 0; i < result.return_count(); ++i) values.push_back(result.get<sol::object>(i));
        return values;
    } catch (const std::exception& exception) {
        instance->faulted = true;
        SetError(instance->failure, instance->Context() + " [method=" + method + "]: " + exception.what());
    } catch (...) {
        instance->faulted = true;
        SetError(instance->failure, instance->Context() + " [method=" + method + "]: unknown callback exception");
    }
    return failure(instance->failure);
}

UiDispatchResult LuaRuntime::Impl::UiDispatcher::Dispatch(UiOwner owner, UiCallback callback,
    const UiEvent& event, std::string& error) {
    error.clear();
    const auto locked = link.lock();
    if (!locked || !locked->runtime) return UiDispatchResult::Dropped;
    auto& runtime = *locked->runtime;
    if (!runtime.state || !runtime.Ui() || runtime.uiSuspended || runtime.shuttingDown)
        return UiDispatchResult::Dropped;
    const auto entry = std::find_if(runtime.uiCallbacks.begin(), runtime.uiCallbacks.end(),
        [&](const auto& value) { return value.token == callback && value.owner == owner; });
    if (entry == runtime.uiCallbacks.end()) return UiDispatchResult::Dropped;
    Instance* instance = nullptr;
    for (const auto handle : runtime.instances.Handles()) {
        auto* candidate = runtime.instances.Get(handle).get();
        if (candidate->uiOwner == owner) { instance = candidate; break; }
    }
    if (!instance || !instance->started || instance->faulted || instance->destroying ||
        (instance->desc.owner && !instance->desc.owner->IsValid())) return UiDispatchResult::Dropped;
    if (instance->active) {
        error = instance->Context() + " [UI source=" + event.sourcePath + ", target=" + event.targetId +
            ", event=" + event.type + "]: reentrant script receiver; event dropped";
        log::Lua().warn("{}", error);
        return UiDispatchResult::Dropped;
    }
    Scope scope(runtime, instance->desc.owner ? instance->desc.owner->GetScene() : nullptr);
    OwnerScope ownerScope(runtime, instance);
    instance->active = true;
    struct Reset { bool& active; ~Reset() { active = false; } } reset{instance->active};
    try {
        // Pin locally: Disconnect/Close can synchronously erase the registry entry.
        sol::protected_function function = entry->function;
        sol::table payload = runtime.state->create_table();
        payload["document"] = event.document;
        payload["sourcePath"] = event.sourcePath;
        payload["targetId"] = event.targetId;
        payload["currentTargetId"] = event.currentTargetId;
        payload["type"] = event.type;
        payload["inputSequence"] = event.inputSequence;
        payload["parameters"] = StateValueToLua(*runtime.state, StateValue(event.parameters));
        const auto context = instance->Context() + " [UI source=" + event.sourcePath +
            ", target=" + event.targetId + ", event=" + event.type + "]";
        if (ProtectedCall(function, error, context, payload)) return UiDispatchResult::Delivered;
    } catch (const std::exception& exception) {
        SetError(error, instance->Context() + " [UI source=" + event.sourcePath + ", target=" +
            event.targetId + ", event=" + event.type + "]: " + exception.what());
    } catch (...) {
        SetError(error, instance->Context() + " [UI source=" + event.sourcePath + ", target=" +
            event.targetId + ", event=" + event.type + "]: unknown C++ exception");
    }
    instance->faulted = true;
    instance->failure = error;
    return UiDispatchResult::OwnerFaulted;
}

void LuaRuntime::Impl::RegisterUi() {
    auto facade = state->create_named_table("UI");
    facade.set_function("CreateModel", [this](const std::string& name, sol::object initial,
        sol::optional<sol::object> declared) {
        std::string error;
        UiOwner owner;
        UiModel model;
        if (!UiOwnerForCall(owner, error, true)) return UiResult(false, model, error);
        UiModelDesc desc;
        desc.name = name;
        const bool explicitSchema = declared && declared->get_type() != sol::type::nil;
        if (explicitSchema) {
            StateValue schemaValue;
            if (!LuaToStateValue(StateValueInput(*declared), schemaValue, error) ||
                !lua_ui_detail::ReadSchema(schemaValue, desc.schema, error)) return UiResult(false, model, error);
        }
        if (!lua_ui_detail::ReadValue(StateValueInput(initial), explicitSchema ? &desc.schema : nullptr, desc.initialValues, error) ||
            (!explicitSchema && !InferUiSchema(desc.initialValues, desc.schema, error)))
            return UiResult(false, model, error);
        // Allocate bookkeeping before native registration so allocation failure cannot orphan a model.
        uiModels.push_back({owner, {}, desc.schema});
        bool ok = false;
        try { ok = Ui()->CreateModel(owner, desc, model, error); }
        catch (...) { uiModels.pop_back(); throw; }
        if (ok) uiModels.back().model = model;
        else uiModels.pop_back();
        return UiResult(ok, model, error);
    });
    facade.set_function("LoadDocument", [this](sol::object source) {
        std::string error;
        UiOwner owner;
        UiDocument document;
        if (!UiOwnerForCall(owner, error, true)) return UiResult(false, document, error);
        UiDocumentDesc desc;
        for (const auto& record : uiModels) if (record.owner == owner) desc.models.push_back(record.model);
        bool ok = false;
        if (source.get_type() == sol::type::string)
            ok = Ui()->LoadDocument(owner, source.as<std::string>(), desc, document, error);
        else if (source.get_type() == sol::type::table) {
            const auto table = source.as<sol::table>();
            const auto id = table.raw_get<sol::object>("id");
            const auto path = table.raw_get<sol::object>("path");
            const auto uuid = id.get_type() == sol::type::string ? AssetUuid::Parse(id.as<std::string>()) : std::nullopt;
            if (!uuid || (path.get_type() != sol::type::nil && path.get_type() != sol::type::string))
                error = "UI.LoadDocument AssetReference requires a valid string id and optional string path";
            else {
                AssetReference asset;
                asset.id = *uuid;
                if (path.get_type() == sol::type::string) asset.path = path.as<std::string>();
                ok = Ui()->LoadDocument(owner, asset, desc, document, error);
            }
        } else error = "UI.LoadDocument expects a catalog logical path or AssetReference table";
        return UiResult(ok, document, error);
    });
    auto document = state->new_usertype<UiDocument>("UiDocument", sol::no_constructor);
    document.set_function("Show", [this](UiDocument token, sol::optional<std::string> mode) {
        const auto name = mode.value_or("interactive");
        if (name != "hud" && name != "interactive" && name != "modal")
            return std::make_tuple(false, std::string("UI Show mode must be hud, interactive or modal"));
        return UiMutate(&UiSubsystem::Show, token, name == "hud" ? UiDocumentMode::Hud :
            name == "modal" ? UiDocumentMode::Modal : UiDocumentMode::Interactive);
    });
    document.set_function("Hide", [this](UiDocument token) { return UiMutate(&UiSubsystem::Hide, token); });
    document.set_function("Close", [this](UiDocument token) { return UiMutate(&UiSubsystem::Close, token); });
    document.set_function("Reload", [this](UiDocument token) { return UiMutate(&UiSubsystem::RequestReload, token); });
    document.set_function("IsValid", [this](UiDocument token) { return Ui() && Ui()->IsValid(token); });
    document.set_function("IsVisible", [this](UiDocument token) { return Ui() && Ui()->IsVisible(token); });
    document.set_function("GetElement", [this](UiDocument token, const std::string& id) {
        UiOwner owner; UiElement element; std::string error;
        const bool ok = UiOwnerForCall(owner, error) && Ui()->FindElement(owner, token, id, element, error);
        return UiResult(ok, element, error);
    });
    document.set_function("On", [this](UiDocument token, const std::string& id,
        const std::string& event, sol::protected_function function) {
        UiOwner owner; UiSubscription subscription; UiCallback callback; std::string error;
        if (!function.valid()) return UiResult(false, subscription, "UI On requires a function callback");
        if (!UiOwnerForCall(owner, error, true) || !UiRuntimeAccess::ReserveCallback(*Ui(), owner, callback, error))
            return UiResult(false, subscription, error);
        bool ok = false;
        try {
            uiCallbacks.push_back({callback, owner, std::move(function)});
            ok = UiRuntimeAccess::Subscribe(*Ui(), owner, token, id, event, callback, subscription, error);
        } catch (...) {
            UiRuntimeAccess::CancelCallback(*Ui(), owner, callback);
            std::erase_if(uiCallbacks, [&](const auto& entry) { return entry.token == callback; });
            throw;
        }
        if (!ok) {
            UiRuntimeAccess::CancelCallback(*Ui(), owner, callback);
            std::erase_if(uiCallbacks, [&](const auto& entry) { return entry.token == callback; });
        }
        return UiResult(ok, subscription, error);
    });
    auto model = state->new_usertype<UiModel>("UiModel", sol::no_constructor);
    model.set_function("Get", [this](UiModel token, const std::string& variable) {
        UiOwner owner; StateValue value; std::string error;
        const bool ok = UiOwnerForCall(owner, error) && Ui()->GetModelValue(owner, token, variable, value, error);
        return std::make_tuple(ok ? StateValueToLua(*state, value) : sol::make_object(*state, sol::nil), error);
    });
    model.set_function("Set", [this](UiModel token, const std::string& variable, sol::object object) {
        UiOwner owner; std::string error;
        if (!UiOwnerForCall(owner, error)) return std::make_tuple(false, error);
        const auto found = std::find_if(uiModels.begin(), uiModels.end(),
            [&](const auto& record) { return record.model == token && record.owner == owner; });
        if (found == uiModels.end() || !found->schema.fields.contains(variable))
            return std::make_tuple(false, std::string("UI model/variable does not belong to the active owner"));
        StateValue value;
        const bool ok = lua_ui_detail::ReadValue(StateValueInput(object), &found->schema.fields.at(variable), value, error) &&
            Ui()->SetModelValue(owner, token, variable, value, error);
        return std::make_tuple(ok, error);
    });
    auto element = state->new_usertype<UiElement>("UiElement", sol::no_constructor);
    element.set_function("IsValid", [this](UiElement token) { return Ui() && Ui()->IsValid(token); });
    element.set_function("SetText", [this](UiElement token, const std::string& text) { return UiMutate(&UiSubsystem::SetText, token, text); });
    element.set_function("SetInnerRml", [this](UiElement token, const std::string& text) { return UiMutate(&UiSubsystem::SetInnerRml, token, text); });
    element.set_function("SetValue", [this](UiElement token, const std::string& text) { return UiMutate(&UiSubsystem::SetFormValue, token, text); });
    element.set_function("GetValue", [this](UiElement token) {
        UiOwner owner; std::string value, error;
        const bool ok = UiOwnerForCall(owner, error) && Ui()->GetFormValue(owner, token, value, error);
        return UiResult(ok, value, error);
    });
    element.set_function("SetClass", [this](UiElement token, const std::string& name, bool enabled) { return UiMutate(&UiSubsystem::SetClass, token, name, enabled); });
    element.set_function("SetProperty", [this](UiElement token, const std::string& name, const std::string& value) { return UiMutate(&UiSubsystem::SetProperty, token, name, value); });
    element.set_function("Focus", [this](UiElement token) { return UiMutate(&UiSubsystem::Focus, token); });
    auto subscription = state->new_usertype<UiSubscription>("UiSubscription", sol::no_constructor);
    subscription.set_function("Disconnect", [this](UiSubscription token) { return UiMutate(&UiSubsystem::Disconnect, token); });
}
