return {
    root = "choose",
    nodes = {
        { id = "choose", kind = "Selector", children = {"pulse", "idle"} },
        { id = "pulse", kind = "Sequence", children = {"enabled", "wait"} },
        { id = "enabled", kind = "Condition", script = "game/ai/enabled.condition.lua" },
        { id = "wait", kind = "Action", script = "game/ai/pulse.action.lua", parameters = {seconds = 1} },
        { id = "idle", kind = "Action", script = "game/ai/idle.action.lua" },
    },
}
