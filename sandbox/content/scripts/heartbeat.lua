-- The first script the engine runs, and the M8.1 done-when test.
--
-- It moves nothing. Moving an entity needs the component access of #261 and
-- the engine API of #262, and neither exists yet. What this proves is the
-- pipeline: a .lua cooks, the runtime finds it by GUID, and the host calls it
-- on the fixed step.
--
-- The count is the interesting part. It goes up once for each step and never
-- once for each frame, so a run at any frame rate reaches the same number in
-- the same simulated time. That is what issue #245 bought.

local steps = 0
local next_report = 0.0

-- How long to wait between reports, in simulated seconds.
local report_every = 2.0

function on_start()
    log.info("heartbeat.lua started")
end

function on_update(seconds)
    steps = steps + 1
    if seconds >= next_report then
        log.info(string.format("heartbeat: %d steps at %.2f simulated seconds", steps, seconds))
        next_report = seconds + report_every
    end
end

function on_destroy()
    log.info(string.format("heartbeat.lua stopped after %d steps", steps))
end
