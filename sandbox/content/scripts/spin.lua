-- Turns an entity on the axis its Spin component names.
--
-- This was engine C++ until M8.6. It is the whole of what sandbox::update did,
-- and moving it here is half of what that increment set out to prove: a game
-- type the engine never heard of reaches a script through the reflection
-- descriptors, which is rule 4.5 working from the outside.
--
-- The entity needs a Spin component beside its ScriptComponent. One without a
-- Spin is not an error. It simply does not turn, the same as before.

local two_pi = 2.0 * math.pi

function on_start()
    if entity:get("Spin") == nil then
        log.warn(string.format("spin.lua runs on %s, which carries no Spin.",
                               entity:name() or "an unnamed entity"))
    end
end

function on_update(seconds)
    -- Read on each step rather than held from on_start, so a turn changed in
    -- the inspector takes effect while the game runs.
    local spin = entity:get("Spin")
    if spin == nil or spin.seconds_per_turn <= 0 then
        return
    end

    -- The angle follows the elapsed time rather than adding up each step, so
    -- two runs of the same length agree and a slow frame drifts nothing. The
    -- seconds are simulated seconds, which is what makes that true. See
    -- DESIGN.md section 9.
    --
    -- quat_from_axis_angle normalizes the axis and answers identity for an axis
    -- of no direction, so a Spin of zero axis needs no guard here.
    local angle = two_pi * seconds / spin.seconds_per_turn
    entity:set("Transform", { rotation = quat_from_axis_angle(spin.axis, angle) })
end
