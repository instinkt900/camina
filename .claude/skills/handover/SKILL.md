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

## Gather the state first, do not write it from memory

Run these and write from what they return. A handover with a wrong issue number
or a stale branch is worse than none, because the next session trusts it.

```bash
git log --oneline -8
git status --short
gh pr list --state open --json number,title,headRefName --jq '.[]|"#\(.number) \(.title) [\(.headRefName)]"'
gh api "repos/instinkt900/camina/milestones?state=all" \
  --jq '.[]|select(.state=="open" or .open_issues>0)|"\(.title): \(.state) open=\(.open_issues)"'
gh issue list --state open --limit 40 --json number,title,labels,milestone \
  --jq '.[]|"#\(.number) [\(.milestone.title // "none")] \(.title)"'
```

For any pull request left open, say whether its checks were green and whether a
review had arrived. Query it rather than recalling it.

If the build state is claimed, check it:

```bash
ctest --preset conan-relwithdebinfo 2>&1 | tail -3
./build/RelWithDebInfo/bin/runtime --offscreen --resolution 1280x720 --frames 120 \
    --no-watch --screenshot /tmp/handover-check.png
```

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
   it is green, and what it still needs. Anything left uncommitted on a branch.

5. **What the next work is.** The next milestone or issue, its issues if they
   exist, and anything already settled about how to approach it. Say which
   decisions the user has already made, so they are not asked twice.

6. **Loose ends.** Issues filed during the work that were deliberately not fixed,
   with one line each on why they can wait. Say plainly that none of them block,
   when that is true.

7. **Things that cost time, and would again.** The traps. Each one costs an hour
   and looks like nothing in a diff, which is why they earn space here. Only put
   something here if it is **not** already in `CLAUDE.md`; if it is durable, put
   it in `CLAUDE.md` and leave it out of this file.

8. **The state of the build.** Test count, lint, format, docs, containment, and
   whether the offscreen capture moved. Give the capture command. Say what is
   fetched locally and not in git, such as Sponza.

9. **What is verified by reading rather than by running.** Anything believed
   correct that no test covers, so the next session does not mistake it for
   proven.

## Rules that keep it honest

- **Never quote an issue or pull request number a command has not returned.**
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
