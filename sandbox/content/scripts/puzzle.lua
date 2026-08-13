-- The sandbox game. Knock the stack into the goal, and win.
--
-- This runs on the goal volume, because a trigger event reaches the volume and
-- not the thing that crossed it. See DESIGN.md section 10 M8.
--
-- The whole game is here: the throw, the win, and the reset. One script owns
-- all three because they share the same state, and splitting them would need
-- two scripts to talk to each other through globals.
--
-- Authored in Lua first and never in C++, which is what M8.6 asked for. A port
-- proves the binding works. It cannot prove the binding is pleasant to write,
-- because a port already knows the answer it wants.

-- The crates the puzzle is about. The rest of the room is scenery.
local stack_names = { "stack crate 0", "stack crate 1", "stack crate 2" }

-- Where each one started, so the reset can put it back.
local homes = {}

-- The crates the throw has made. The reset takes them away again.
local thrown = {}

-- Which stack crates are inside the goal now, keyed by entity id.
local inside = {}

-- How fast a crate leaves the camera, in metres each second.
local throw_speed = 12.0

-- How far in front of the camera it appears. A body created inside the near
-- plane is a crate that fills the screen for one frame.
local throw_offset = 1.5

local function count(set)
    local n = 0
    for _ in pairs(set) do
        n = n + 1
    end
    return n
end

function on_start()
    -- An array rather than a table keyed by name, so ipairs walks it in a fixed
    -- order. A string-keyed table walks in an order Lua seeds from the clock,
    -- and a run that depended on it would not reproduce. See script/bindings.h.
    for _, name in ipairs(stack_names) do
        local crate = world.find(name)
        if crate == nil then
            log.warn(string.format("puzzle.lua found no crate named %s.", name))
        else
            homes[#homes + 1] = { entity = crate, position = crate:world_position() }
        end
    end

    -- The win is deliberately not cleared here. on_start runs again on every
    -- reload, so clearing it would wipe a solved puzzle each time the file is
    -- saved, which is the opposite of what putting the win on a component
    -- bought. The scene ships it false and the reset is what clears it. See
    -- DESIGN.md section 10 M8.
    log.info(string.format("Puzzle ready. Throw with F at %d crates, reset with R.", #homes))
end

local function throw()
    local from = camera.position()
    local forward = camera.forward()
    if from == nil or forward == nil then
        log.warn("puzzle.lua cannot throw, because this step has no camera.")
        return
    end

    local crate = world.instance("thrown_crate.prefab")
    if crate == nil then
        return
    end

    if not crate:add_body() then
        log.error("The thrown crate got no body, so it will not fall.")
        return
    end

    -- teleport and not a Transform write, even though the body has only just
    -- been made. A dynamic body owns its pose, so the next step throws away
    -- anything written to the component. The host warns when a script tries it.
    crate:teleport(from + forward * throw_offset)
    crate:set_velocity(forward * throw_speed)

    thrown[#thrown + 1] = crate
end

local function reset()
    for _, home in ipairs(homes) do
        -- teleport rather than writing the Transform. A dynamic body owns its
        -- pose, so the next step would overwrite anything written to the
        -- component and the crate would drop straight back where it fell.
        home.entity:teleport(home.position)
    end

    for _, crate in ipairs(thrown) do
        world.destroy(crate)
    end
    thrown = {}

    inside = {}
    entity:set("Goal", { won = false })
    log.info("The room is back.")
end

function on_update(seconds)
    -- The press edge rather than the key being down. Holding the key would
    -- otherwise fill the room with crates in one second.
    if input.pressed("throw") then
        throw()
    end
    if input.pressed("reset") then
        reset()
    end

    local goal = entity:get("Goal")
    if goal == nil or goal.won or count(inside) == 0 then
        return
    end

    -- A crate merely passing through the goal has not landed in it, so the win
    -- counts the ones that have settled. A sleeping body is what settled means.
    local settled = 0
    for _, home in ipairs(homes) do
        if inside[home.entity.id] and not home.entity:is_awake() then
            settled = settled + 1
        end
    end

    -- The scene says how many are needed, so raising the bar is an edit to a
    -- component and not to this file.
    if settled >= goal.needed then
        entity:set("Goal", { won = true })
        log.info(string.format("You win. %d crate(s) came to rest in the goal.", settled))
    end
end

function on_trigger(other, began)
    -- The volume hears this, and `other` is whatever crossed it. A thrown crate
    -- crossing the goal is not a win, so only the stack is counted.
    for _, home in ipairs(homes) do
        if home.entity.id == other.id then
            inside[other.id] = began or nil
            return
        end
    end
end
