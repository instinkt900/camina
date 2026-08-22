-- The sound a crate makes when it lands on something.
--
-- A script of its own rather than a branch in puzzle.lua, because puzzle.lua
-- runs on the goal volume and a trigger never hears a contact. A contact
-- reaches the two bodies that touched, so this has to run on the crate.
--
-- It shares no state with the puzzle, which is what makes a second script the
-- right answer here. DESIGN.md section 10 M8 says the game is one script
-- because the throw, the win and the reset share state. These do not.

-- The sound, by source path.
local thud = "sounds/thud.wav"

-- How long to wait before this crate may thud again, in simulated seconds.
--
-- A crate that lands does not touch once. It bounces, it settles, and a stack
-- of them touches every neighbour, so a thud for every contact is a burst
-- rather than an impact. This is what turns that back into one sound.
local quiet_seconds = 0.12

-- When this crate may next be heard. Simulated seconds, so it moves with the
-- step and not with the frame rate.
local next_at = 0.0

-- Now, in simulated seconds. on_update is what advances it.
local now = 0.0

-- Below this the crate is settling rather than landing, in metres each second.
--
-- Without it a stack that is still creeping thuds forever, and a crate resting
-- on the floor is never quite still.
local quiet_speed = 1.2

-- The speed a landing has to reach to be heard at full volume.
local loud_speed = 8.0

function on_update(seconds)
    now = now + seconds
end

function on_contact(other, began)
    -- The touch starting is the impact. The touch ending is the crate leaving,
    -- and nothing makes a sound by leaving.
    if not began then
        return
    end
    if now < next_at then
        return
    end

    local velocity = entity:velocity()
    if velocity == nil then
        return
    end

    -- How hard it hit. A crate the player threw is loud and a crate settling
    -- into the stack is not, and one sound at one volume cannot tell them
    -- apart.
    local speed = math.sqrt(velocity.x * velocity.x + velocity.y * velocity.y
                            + velocity.z * velocity.z)
    if speed < quiet_speed then
        return
    end

    local loudness = math.min(speed / loud_speed, 1.0)
    local at = entity:world_position()
    if at == nil then
        return
    end

    audio.play(thud, { bus = "effects", volume = loudness, x = at.x, y = at.y, z = at.z })
    next_at = now + quiet_seconds
end
