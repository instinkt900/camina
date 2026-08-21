---
name: handover
description: Write handover.md at the repository root, recording where the work stopped and what a next session should do first. Use at the end of a working session, before a break, or whenever the user asks for a handover or a summary to continue from. The file is a note between sessions and is deleted once it has been read.
---

# Handover

Write `handover.md` at the repository root. It is read at the start of the next
session, by a Claude with **no memory of this one**, and it is deleted once read.

## What it is for, and what it is not

`CLAUDE.md` is the standing brief and `DESIGN.md` is the architecture. Both are
already read at the start of a session.

**So the handover holds only what those two do not**: where the work stopped,
what is waiting, and the few things a next session would otherwise have to find
out the hard way. Anything durable belongs in `CLAUDE.md` or `DESIGN.md` instead,
and should be put there rather than here.

Do not restate the project, the hard rules, the build commands, or the flow. A
handover that repeats `CLAUDE.md` buries the part that matters.

## Write it from what you already know

**This is a context dump, not an investigation.** Write down what this session
holds, so the next one starts where this one stopped. It is the last thing a
session does, and it has to be quick.

**Do not analyze the state of the repository. Do not run extra work to fill it
in.** No test run, no build, no offscreen capture, no tracker queries for work
this session never touched, no reading files to check a claim. If the session
did not already establish something, the handover does not need it.

Two cheap commands are allowed, because a wrong sha or a forgotten uncommitted
file sends the next session the wrong way:

```bash
git log --oneline -5
git status --short
```

Everything else comes from the session. When you are unsure of an issue number,
a pull request number, or whether a check went green, **say so in the file**
rather than going to look. "PR #412 was green when last seen" is useful. A
number nobody checked, written as fact, is worse than a gap.

## Write about the project, not about the session

A session holds more than project work. It may include questions about the
tooling, work on the `.claude/` directory itself, options weighed and dropped,
and discussion that changed nothing. **None of that belongs in the handover.**

The test is whether the next session needs it to carry the engineering forward.

**In:** anything that changed the repository or what happens to it. Engine code,
content, the build, CI that gates a pull request, the documents, the tracker, and
the decisions behind them. A decision the user made that constrains later work
belongs here too, so it is not put to them a second time.

**Out:** how the session was run, questions about the assistant or its features,
changes under `.claude/`, and approaches considered without being taken. A
handover is a note about the project, not a log of the conversation.

## What the document holds

Use these headings, in this order. Drop a heading that has nothing under it
rather than writing "none".

1. **A first line saying when it was written and what `main` is at**, by sha.
   Then one line pointing at `CLAUDE.md` and saying what this file adds.

2. **Where the work stopped.** The milestone or task, and a table of what landed:
   increment, issue, pull request, and one line on what it built. Say plainly
   whether it is complete.

3. **Do these first.** The two or three things a next session should do before
   anything else, in order, with the reason for each. This is the most useful
   section in the file. Include checking the next milestone's GitHub description
   against `DESIGN.md` §10 when a milestone is starting, because the two have
   drifted before.

4. **What is in flight.** Any open pull request, its number, its branch, whether
   it was green when last seen, and what it still needs. Anything left
   uncommitted.

5. **What the next work is.** The next milestone or issue, its issues if they
   exist, and anything already settled about how to approach it. Say which
   decisions the user has already made, so they are not asked twice.

6. **Loose ends.** Issues filed during the work that were deliberately not fixed,
   with one line each on why they can wait. Say plainly that none of them block,
   when that is true.

7. **Things that cost time, and would again.** The traps this session hit. Each
   one costs an hour and looks like nothing in a diff, which is why they earn
   space here. Only put something here if it is **not** already in `CLAUDE.md`;
   if it is durable, put it in `CLAUDE.md` and leave it out of this file.

8. **The state of the build.** What this session last saw: the test result, the
   lint, the format check, and whether the offscreen capture moved. Report the
   last observation and when it was taken. Do not run any of it again to fill
   this in, and say plainly when the session never checked.

9. **What is verified by reading rather than by running.** Anything believed
   correct that no test covers, so the next session does not mistake it for
   proven.

## Rules that keep it honest

- **Never state an issue or pull request number as fact when this session did not
  see it.** Mark it as unverified, or leave it out.
- **Say what was measured and what was assumed.** A number with no command behind
  it is a guess, and the next session cannot tell the difference.
- **An empty finding is a finding.** If an exercise was run and turned up nothing,
  say it was run. Silence reads as "not done".
- **Name what is unfinished plainly.** A handover that reads as if everything is
  done wastes the next session's first hour.
- Write in the relaxed STE the project uses: active voice, no semicolons, no
  contractions, sentences under 25 words. Use the `ste-writing` skill.

## After writing it

Tell the user the file is written and that it is untracked, so it never reaches a
commit. `handover.md` is a note between sessions rather than part of the
repository. The next session reads it, acts on it, and deletes it.
