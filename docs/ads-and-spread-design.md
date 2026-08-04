# ADS and spread in a flat-to-VR port

Two questions from playtesting, with different answers. The first is a
mechanism problem with a well-established solution. The second is a **balance**
question wearing a technical costume, and the honest answer is "not yet".

## 1. How do you detect ADS with motion controls?

In a VR-first shooter there is no ADS *state* at all. The sights are geometry,
your hand holds the gun, and aiming down them is just what happens when you
raise it to your eye. Onward, Pavlov, H3VR and Contractors all work this way.

But Black Ops is not VR-first, and its ADS is a real mode with real consequences:
different spread, different recoil, different movement speed, a different FOV,
and animations to enter and leave it. `adsbuttonpressed` exists in the binary,
which confirms it is button-driven. We cannot make ADS emergent without
reimplementing weapon handling — so the mod's job is to *synthesise* the button
from what the player physically did.

The standard flat-to-VR solution, and the right one here:

**ADS is on when the weapon hand is near the dominant eye AND roughly aligned
with the view direction.**

Both terms are needed. Distance alone fires when scratching your face; alignment
alone fires whenever the gun happens to point where you are looking, which is
most of the time. Concretely, with the poses we already have live:

```
d     = |hand_pos - head_pos|                      /* both already in cod units */
align = dot(normalise(hand_fwd), head_fwd)
ads   = (d < D_ON)  && (align > A_ON)
```

with hysteresis — separate on/off thresholds — because a boolean derived from a
noisy continuous signal chatters at the boundary, and a weapon animation
flickering in and out of ADS is far worse than a slightly sticky one.

Two things this needs that do not exist yet:

* a way to press the button (#46, the input path — not yet surveyed);
* the grip-to-muzzle offset (#45), because "near the eye" should mean the rear
  sight, not the palm.

Both thresholds belong in the mod menu (#47), because head and arm proportions
differ and nobody's numbers are everybody's.

## 2. Do we remove spread?

**Not now, and probably not wholesale.**

### Why VR-first titles mostly do not have it

Random spread in a flat shooter is partly a stand-in for something the player's
input cannot express. A mouse-and-keyboard player is not holding a weapon, so
the game models the difficulty of holding one — stance, movement, hip-firing,
breathing — by scattering the bullets. It is a simulation of *aiming difficulty*.

In VR that difficulty is real and already paid: your actual arm is unsteady, the
recoil physically moves the gun, and hip-firing genuinely is harder than
shouldering. So VR-native shooters mostly keep only **mechanical** dispersion —
shotgun pellets, and whatever inherent inaccuracy the weapon has — and drop the
player-state penalties, because the player is now performing them for real.

### Why that argument does not yet apply to us

That reasoning depends on the player physically holding the gun. **We do not
have motion controls yet.** Today the weapon is aimed by stick or mouse, exactly
as in the flat game, so every penalty spread models is still doing its job. If
we removed it now we would not be making the game VR-native, we would just be
making it easier.

There is also a balance point worth stating plainly: BO1 Zombies is tuned around
its weapons, and spread is part of that tuning. Removing it changes the game's
difficulty, which is a legitimate thing to *offer* and a bad thing to *impose*.

### The principled line, when motion controls land

Remove penalties that model **input limitations VR does not have**; keep
dispersion that models **the weapon**.

| Spread source | VR-native call |
|---|---|
| hip-fire penalty (`hipSpread*`) | reduce — hip-firing is now a real physical act the player is doing |
| movement / turn penalty | reduce — the player really is moving their own arm |
| stance (`*Ducked*`, `*Prone*`) | reduce |
| weapon inherent / shotgun pellets | **keep** — that is the gun, not the player |

### The lever

`perk_weapSpreadMultiplier` is in the binary and is a global multiplier on
weapon spread. That makes this a *scalar setting* rather than an on/off
amputation — which is the right shape for something that is a balance choice.
Default 1.0 (untouched), exposed in the mod menu (#47) once it exists.

## 3. What to do first, and why in this order

1. **Make the reticle tell the truth about the spread that exists** (#48). Right
   now it is a fixed size much smaller than the real cone, so it claims accuracy
   the weapon does not have. This is a correctness bug and it is independent of
   every argument above.
2. **Motion controls** (#45, #46). Only after these does the case for cutting
   spread rest on anything real.
3. **Then** revisit spread with the cone actually visible and the gun actually
   in hand, and make it a setting rather than a decision.

Doing 3 before 1 would be tuning a number while unable to see its effect.
