local elapsed
return {
    OnEnter = function()
        elapsed = 0
    end,
    Tick = function(ctx, dt)
        elapsed = elapsed + dt
        if elapsed < ctx.parameters.seconds then
            return BT.Running
        end
        ctx.blackboard.pulses = (ctx.blackboard.pulses or 0) + 1
        Log.info(ctx.entity:GetName() .. " pulse " .. ctx.blackboard.pulses)
        return BT.Success
    end,
    OnExit = function(ctx, reason)
        if reason == "Cancelled" then
            Log.info(ctx.entity:GetName() .. " stopped")
        end
    end,
}
