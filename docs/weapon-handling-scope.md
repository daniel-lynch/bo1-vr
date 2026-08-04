# What the bone drive buys, and what it does not

With `docs/bone-drive-findings.md` settled, the natural question is how much of
real VR weapon handling now falls out. The answer splits cleanly, and the
dividing line is worth stating once because it decides the order of everything:

> **The bone drive controls WHERE THE WEAPON IS. It does not change WHAT THE
> GAME THINKS ABOUT IT.**

Anything that is purely a transform is nearly free. Anything that changes game
state — whether you can fire, what you are holding, whether you still have it —
needs the input/state path (#46) as well, and is a different order of work.

## 1. Two-handed hold — YES, and it is the cheapest of the four

Purely a transform. Position from the dominant hand, orientation from the vector
between the two controller positions, and feed the result to the bone drive as
`desired`. No new engine interaction whatsoever: it is a different *function* of
two poses we already have live and verified.

It is also the single biggest accuracy win available, for the same reason it is
in every VR shooter — a two-point grip removes most of the wobble a one-point
grip has, and it does so physically rather than by damping.

**Caveat, and it is a real one:** we are hiding the viewmodel arms (#51), so
there is no second hand rendered on the foregrip. Until we draw our own hands,
two-handed hold will *feel* right and *look* like the gun floating. That is a
content problem, not an engine one.

## 2. Dual wield — STRUCTURALLY SUPPORTED, for weapons the game already allows

`docs/viewmodel-findings.md` found the akimbo weapon is attached at
**`tag_weapon1`**, a second attach point in the same viewmodel DObj. So the
engine already carries two weapons in the structure we are driving; driving
`tag_weapon` from the right controller and `tag_weapon1` from the left is the
same mechanism applied twice.

What that does NOT give us is making arbitrary weapons akimbo. Which weapons
support it is weapon-def and asset data living in fastfiles, not something the
bone drive reaches.

## 3. Holstering — VISUALLY YES, MECHANICALLY IT NEEDS MORE

Moving the weapon to a hip or shoulder position is just another `desired`
transform, so the visual half is nearly free.

The mechanical half is not. The game still believes the weapon is up and ready:
the player could fire from the "holstered" position, and the fire would come
from the game's aim, not the gun's. A holster that does not stop you firing is a
prop, not a mechanic. Doing it properly means driving the game's own weapon
lower/switch state through the input path (#46).

**One unknown that could bite.** The viewmodel is rendered with a depth hack —
`camera-hook-plan.md` §2.2 records `depthHackNearClip` at GfxViewParms `+0x134`,
which exists precisely so the gun does not clip into walls. A weapon translated
far from its authored position may hit that near-clip behaviour or the
viewmodel's separate depth range, and look wrong at the hip even though the
transform is correct. Worth an early cheap test: translate the weapon a long way
down and see what happens, before building a holster on the assumption it works.

## 4. Throwing the weapon — NO, not by this path

This is the one to be plain about. The bone drive moves the weapon *within the
viewmodel*, which is a view-space construct rendered with its own projection. A
thrown weapon has to stop being a viewmodel attachment and become a **world
entity** with a position, a trajectory and collision. That is not a transform
problem; it is entity spawning and physics, and none of the work so far touches
it.

There is a further problem specific to this game: Zombies has no drop-weapon
mechanic, so the engine support that would exist in a mode where players trade
weapons may not be reachable here at all. Before anyone estimates this, the
question to answer is whether a dropped/world weapon entity can be spawned
client-side at all — and if it can only be done server-side, whether that is
even available to us.

Treat throwing as **out of scope** until something establishes that, and do not
let it ride along in a task with the other three: it is not the same size.

## Suggested order

1. **Two-handed hold** — free once the bone drive works, biggest feel win.
2. **Holster, visual only**, as the depth-hack probe. Cheap, and it answers a
   question the rest depends on.
3. **Dual wield** — same mechanism twice, once one hand is solid.
4. **Holster, mechanical** — after #46 is runtime-proven.
5. **Throwing** — only after a separate feasibility answer.

Note that 1-3 all want our own hand models, which is the first real *content*
dependency this project has had. Everything to date has been code against an
existing binary.
