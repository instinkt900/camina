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

-- The three layouts this game puts on the screen, by source path. The surface
-- loads one on the first show, so nothing else has to know they exist. A path
-- rather than an identity, because a cooked identity is derived and nobody can
-- type one.
local main_menu = "ui/main_menu.mothui"
local hud = "ui/hud.mothui"
local pause_menu = "ui/pause.mothui"

-- What the status line says while the puzzle is unsolved.
local instructions = "Throw with F. Reset with R."

-- Writes one line into a node of a layout. A node the layout does not hold is a
-- layout somebody edited, not a fault worth stopping the game for, so this
-- reports it once and carries on.
local function write_line(layout, node, text)
    local found = ui.find(layout, node)
    if found == nil then
        log.warn(string.format("%s holds no node called %s.", layout, node))
        return
    end
    found:set_text(text)
end

-- Writes the status line of the HUD.
local function say(text)
    write_line(hud, "status", text)
end

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
    say(instructions)
    log.info("The room is back.")
end

-- The three screens this game moves between, declared before the button table
-- names them. A Lua local is in scope only under the line that declares it, and
-- these four call each other and are called from that table.
local show_menu
local start_game
local pause_game
local resume_game

-- What each button does, and what each one says. A press names a layout and a
-- node, so this is keyed by both: three layouts each hold buttons, and a node
-- id is only unique inside the layout that declares it.
--
-- ipairs and a list rather than a string-keyed table, because Lua 5.4 seeds its
-- string hash from the clock and a keyed walk is not reproducible. See
-- DESIGN.md section 9.
local buttons = {
    { layout = hud, node = "throw button", label = "Throw",
      press = function() throw() end },
    { layout = hud, node = "reset button", label = "Reset",
      press = function() reset() end },
    { layout = hud, node = "pause button", label = "Pause",
      press = function() pause_game() end },
    { layout = main_menu, node = "play button", label = "Play",
      press = function() start_game() end },
    { layout = pause_menu, node = "resume button", label = "Resume",
      press = function() resume_game() end },
    -- Leaving for the menu puts the room back, because Play starts a run and a
    -- half-finished one behind the title screen is not a run anybody asked for.
    { layout = pause_menu, node = "menu button", label = "Main menu",
      press = function() reset() show_menu() end },
}

-- Writes the label of every button of one layout.
--
-- The label is a node inside button.mothui, so it is named through the
-- reference that stands that button up. Every reference holds a child called
-- `label`, and a node id is unique only inside the layout that declares it, so
-- the bare name would answer with whichever reference comes first. See
-- DESIGN.md section 8.4.
--
-- A reload builds the nodes again from the layout file, so every label goes
-- back to the authored text. Each show calls this for the layout it put up.
local function label_buttons(layout)
    for _, button in ipairs(buttons) do
        if button.layout == layout then
            write_line(layout, button.node .. "/label", button.label)
        end
    end
end

-- What the status line says now.
local function status_text()
    local goal = entity:get("Goal")
    if goal ~= nil and goal.won then
        return "Solved. Reset with R."
    end
    return instructions
end

-- Writes the line of the HUD that the running game changes.
--
-- It answers nothing when the HUD is not up. The step that puts the main menu
-- up runs on_update once more before the pause takes effect, and the HUD is not
-- even loaded then.
local function refresh()
    if not ui.visible(hud) then
        return
    end
    local goal = entity:get("Goal")
    local needed = goal ~= nil and goal.needed or 0
    write_line(hud, "score", string.format("In the goal: %d of %d", count(inside), needed))
end

-- Writes everything this game owns in one layout.
--
-- Every one of these is a value the file does not carry: a label lives inside
-- `button.mothui` and is the same word for every reference, and the two HUD
-- lines are the game's own state. So a rebuild loses all of them, and this is
-- what puts them back.
local function write_own(layout)
    label_buttons(layout)
    if layout == hud then
        say(status_text())
        refresh()
    end
end

-- Shows one layout and writes what it owns, or reports that this build has no UI.
local function put_up(layout)
    if not ui.show(layout) then
        return false
    end
    write_own(layout)
    return true
end

-- **A rebuilt layout carries the text its file carries.** A hot reload builds
-- the node tree again, and so does an image reload, so everything written above
-- is gone. The engine cannot put it back, because it never knew which values a
-- script chose. It reports the layout instead and this writes them again.
--
-- It arrives on the frame clock while the game is paused, which is when a
-- person edits a menu. See DESIGN.md section 8.4.
function on_ui_reload(layout)
    write_own(layout)
end

-- The main menu, with the game held. This is where a run starts.
--
-- **A build with no game UI plays instead.** ui.show answers false when nobody
-- bound a surface, which is what with_ui=False gives and what the editor gives
-- today. Pausing with no menu on the screen would be a game nobody can start,
-- because only a button can resume one. See DESIGN.md section 10 M10.
function show_menu()
    if not put_up(main_menu) then
        log.warn("There is no game UI in this build, so the game starts without a menu.")
        start_game()
        return
    end

    ui.hide(hud)
    ui.hide(pause_menu)
    game.pause()
end

-- The game itself, with the HUD over it.
function start_game()
    ui.hide(main_menu)
    ui.hide(pause_menu)
    put_up(hud)
    game.resume()
end

-- The pause menu, over the HUD rather than instead of it. Showing a layout
-- raises it, so the pause menu draws last and answers a click first.
function pause_game()
    if game.paused() then
        return
    end
    if not put_up(pause_menu) then
        return
    end
    game.pause()
end

function resume_game()
    ui.hide(pause_menu)
    game.resume()
end

-- The one callback a paused game runs. It reads the key that resumes and
-- nothing else, because a paused game moves nothing.
--
-- **Only the pause menu answers the key.** The main menu is a paused screen
-- too, and P there would start the game with the title still on the screen.
function on_paused_update()
    if ui.visible(pause_menu) and input.pressed("pause") then
        resume_game()
    end
end

-- This sits below the functions it calls. A Lua local is in scope only under
-- the line that declares it, so the screens have to come first.
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

    -- **Which screen is up is the UI's own state, not a component.** A reload
    -- keeps every layout showing and keeps the game paused, so a save while the
    -- pause menu is open leaves it open. So this puts the main menu up only
    -- when nothing is up yet, which is the first start of a run.
    if ui.visible(main_menu) or ui.visible(hud) or ui.visible(pause_menu) then
        -- A reload built the nodes again, so every label is back to the text
        -- the file carries and every line the game wrote is gone.
        for _, layout in ipairs({ main_menu, hud, pause_menu }) do
            if ui.visible(layout) then
                write_own(layout)
            end
        end
    else
        show_menu()
    end

    log.info(string.format("Puzzle ready. Throw with F at %d crates, reset with R, "
                           .. "pause with P. Each is a button as well.", #homes))
end

function on_ui_press(pressed_layout, node)
    for _, button in ipairs(buttons) do
        if button.layout == pressed_layout and button.node == node then
            button.press()
            return
        end
    end
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

    -- The pause key. `on_paused_update` reads the same key to resume, because
    -- this line never runs while the menu is up.
    if input.pressed("pause") then
        pause_game()
    end

    -- The HUD carries a count the running game changes, so it is written on
    -- every step rather than only when something happens.
    refresh()

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
        say(string.format("You win. %d crate(s) came to rest.", settled))
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
