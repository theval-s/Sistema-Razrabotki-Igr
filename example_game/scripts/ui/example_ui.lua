local hud, pause, inventory, hudModel, settings, inventoryModel, controls
local items = {}
local effectClip = "375c7852-3506-4aba-a2bc-2006c5472001"

local function playEffect(bus)
    local voice, message
    if bus == "UI" then
        voice, message = Audio.Play2D(effectClip, { bus = "UI", volume = 0.35 })
    else
        -- A fixed-position one-shot overlaps the following hum without restarting it.
        voice, message = Audio.PlayAt(effectClip, Vector3.new(0, 3.8, 0), { bus = "SFX", volume = 0.7, minDistance = 2 })
    end
    if not voice then Log.warning(message) end
end

local function refreshItems()
    assert(inventoryModel:Set("items", items))
end

function OnStart()
    hudModel = assert(UI.CreateModel("example_hud", { player = "Player / Игрок", health = 100, seconds = 0 }))
    settings = assert(UI.CreateModel("example_settings", { player = "Player / Игрок", volume = 75, music = 50, sfx = 75, ui = 75 }))
    Audio.SetMasterVolume(0.75)
    Audio.SetBusVolume("Music", 0.5)
    Audio.SetBusVolume("SFX", 0.75)
    Audio.SetBusVolume("UI", 0.75)
    if Audio.IsAvailable() then
        local voice, message = Audio.Play2D("375c7852-3506-4aba-a2bc-2006c5472003", {
            loading = "Streamed", bus = "Music", scope = "session", looping = true, volume = 0.3
        })
        if not voice then Log.warning(message) end
    else
        Log.warning(Audio.GetDiagnostic())
    end
    inventoryModel = assert(UI.CreateModel("example_inventory", { items = {} }, {
        type = "object", fields = { items = { type = "array", element = {
            type = "object", fields = { name = "string", count = "number", description = "string" }
        } } }
    }))
    for i = 1, 12 do
        items[i] = { name = "Supply " .. i, count = i, description = "A useful travel supply." }
    end
    refreshItems()
    hud = assert(UI.LoadDocument("game/ui/hud.rml"))
    pause = assert(UI.LoadDocument("game/ui/pause.rml"))
    inventory = assert(UI.LoadDocument("game/ui/inventory.rml"))
    for _, entry in ipairs({ { "volume", "Master" }, { "music", "Music" }, { "sfx", "SFX" }, { "ui", "UI" } }) do
        local field, bus = entry[1], entry[2]
        assert(pause:On(field, "change", function()
            local value = settings:Get(field) -- Get also returns an error string; keep only the value.
            local gain = tonumber(value) / 100
            if bus == "Master" then Audio.SetMasterVolume(gain) else Audio.SetBusVolume(bus, gain) end
        end))
    end
    assert(pause:On("test-audio", "click", function() playEffect("UI") end))
    assert(pause:On("resume", "click", function()
        assert(pause:Hide())
        assert(Time.SetPaused(false))
    end))
    assert(inventory:On("close", "click", function() assert(inventory:Hide()) end))
    assert(inventory:On("clear", "click", function() items = {}; refreshItems() end))
    assert(inventory:On("add", "click", function()
        items[#items + 1] = { name = "Potion", count = 1, description = "Restores health." }
        refreshItems()
    end))
    assert(hud:Show("hud"))
    controls = assert(Input.GetContext("ExampleUI"))
    controls:BindAction("ExamplePause", PhysicalKey.P)
    controls:BindAction("ExampleInventory", PhysicalKey.I)
    controls:BindAction("ExampleSound", PhysicalKey.T)
    -- Reapply on every scripting-session start, including after loading a save.
    -- Menus temporarily release mouse-look without changing this request.
    Input.SetCursorMode(CursorMode.Locked)
end

function OnUpdate(dt)
    assert(hudModel:Set("seconds", math.floor(Time.SimulationTime())))
    assert(hudModel:Set("player", settings:Get("player")))
    if controls:Pressed("ExampleSound") then playEffect("SFX") end
    if controls:Pressed("ExamplePause") then
        assert(inventory:Hide())
        assert(pause:Show("modal"))
        assert(Time.SetPaused(true))
    elseif controls:Pressed("ExampleInventory") then
        if inventory:IsVisible() then assert(inventory:Hide()) else assert(inventory:Show("interactive")) end
    end
end
