# Working with Claude Code on this repo

## Before the first session

Do not run `/init`. It generates a `CLAUDE.md`, and this repo already has one that
carries decisions you do not want regenerated from a code scan.

Claude Code picks up `CLAUDE.md` automatically. It does not automatically read
`docs/`, so the opening prompt points it there.

## Opening prompt

Paste this first, in a fresh session. It verifies understanding before anything
gets written.

```
Read CLAUDE.md, README.md, docs/payload-contract.md and docs/decisions.md
before doing anything else.

Then, without changing any code, tell me:
1. What this project is, in three sentences.
2. Which decisions are settled and which are still open.
3. Anything in console/index.html that already contradicts the rules in
   CLAUDE.md.

Do not pick a bridge transport. Do not change the payload contract. Do not add
a build step, a framework, or any dependency. Wait for me before writing code.
```

If the summary it gives back is wrong, fix it there rather than letting it start
coding from a wrong model of the project.

## Candidate first tasks

Pick one. The first is the only one that is fully unblocked.

### A. Parser test suite, no open decisions in the way

```
Extract the parse() function from console/index.html into console/parser.js as
an ES module, and have index.html import it. Then write console/parser.test.js
covering, at minimum:

- a valid SOS, OK and TEL frame
- an unknown frame type
- a wrong schema version
- a malformed device id
- latitude and longitude out of range
- 0|0 coordinates, which must parse successfully as a NO FIX distress call
- empty coordinates, same
- an unknown category letter
- a missing trailing field

Every rejection must return a human readable reason. Use node:test so there is
no new dependency. Show me the plan before you write anything.
```

This matters more than it looks. The parser is the one piece where a bug means a
real distress call is discarded, and it is the piece most likely to get quietly
"simplified" later.

### B. Persistence, needs decision D6 first

Do not start this until you have chosen a store. When you do:

```
Incident records currently live in memory and die on refresh. Propose three
options for persisting them, with the tradeoffs for a base station that runs on
one machine at a staffed facility with no guaranteed internet. Do not implement
anything yet.
```

### C. Bridge, needs decision D5 first

Blocked on the licensing call in `docs/decisions.md`. Once decided:

```
Implement the bridge in bridge/ using <the transport you chose>. It reads
frames from the gateway and calls window.feedPacket(line, link) in the
console, and implements window.sendToMesh(frame) for outbound ACK frames.

I have one Heltec V4 and no second node, so it must be testable point to point
with no mesh. Do not write tests that require multiple nodes.
```

## What to watch for

Claude Code will drift toward these unless stopped. Each one is already a rule in
`CLAUDE.md`, which is why the opening prompt makes it read that first.

- Adding npm, a bundler, React, or Tailwind to the console. It is a single file
  that opens from disk on purpose, on a machine that may have no internet.
- Turning the alarm into a toast or a notification banner, usually described as
  making it less intrusive. The intrusiveness is the feature.
- Wrapping the parser in a try/catch that swallows the reason. A silently dropped
  distress packet is the worst failure this system has.
- Treating `0|0` coordinates as invalid input. That is a real distress call from a
  beacon under canopy that has not got a lock.
- Auto-acknowledging on receipt to reduce clicks. That tells a person help is
  coming when nobody has looked at the screen.
- Adding localStorage or sessionStorage.
- Choosing a bridge transport or a database without being asked.

## Habits that help

Ask for a plan before edits to anything in the distress path: parser, alarm state,
acknowledgement. Read the diff on those rather than accepting.

When it proposes something that contradicts `CLAUDE.md`, say so and point at the
rule rather than just rejecting. It updates its behaviour for the rest of the
session that way.

If a decision genuinely should change, change it in `docs/decisions.md` in the
same commit as the code. The log is only useful if it stays true.
