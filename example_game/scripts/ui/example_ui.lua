local hud, pause, inventory, hudModel, settings, inventoryModel, controls
local items = {}

local function refreshItems()
    assert(inventoryModel:Set("items", items))
end

function OnStart()
    hudModel = assert(UI.CreateModel("example_hud", { player = "Player / Игрок", health = 100, seconds = 0 }))
    settings = assert(UI.CreateModel("example_settings", { player = "Player / Игрок", volume = 75 }))
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
    -- Reapply on every scripting-session start, including after loading a save.
    -- Menus temporarily release mouse-look without changing this request.
    Input.SetCursorMode(CursorMode.Locked)
end

function OnUpdate(dt)
    assert(hudModel:Set("seconds", math.floor(Time.SimulationTime())))
    assert(hudModel:Set("player", settings:Get("player")))
    if controls:Pressed("ExamplePause") then
        assert(inventory:Hide())
        assert(pause:Show("modal"))
        assert(Time.SetPaused(true))
    elseif controls:Pressed("ExampleInventory") then
        if inventory:IsVisible() then assert(inventory:Hide()) else assert(inventory:Show("interactive")) end
    end
end
