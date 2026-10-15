# Match HUD controls — from Lola's video (2026-10-14, frames in frames-ad/)

Phone video of her Vita playing Build AA, 30.9s, 1280x720. Aiming HUD
(bottom bar, from frame f-26, the cleanest straight-on shot):

- Move box (fuel count, "4")
- Weapon box ("Cannon Ball")
- **Fire** button (big, labeled)
- **Angle** widget (tab-labeled, numeric value, e.g. 66)
- **Power** widget (tab-labeled, bar + numeric value, e.g. 54)

Tapping Angle or Power EXPANDS it into a wide slider across the
bottom-left (frames f-06 Power, f-16/a-08 Angle), with a value box,
a "->" button, and a green-check (confirm) button at bottom-right.

Slider behavior is NOT absolute tap-to-set and NOT knob-chases-finger:
in a-08 her fingertip sits past the 70 tick while Angle reads 59;
in p-04/p-10 the knob sits ~44% while her finger rests at the
track's right end (value ~49-51). Looks like relative scrubbing
(drag delta drives value), possibly with gain != 1. A crosshair
reticle also appears mid-screen (aim marker; engine-side INPUTSTATE
samples have shown her touch at (516,170) mid-screen in a match).

=> Do not guess the transfer function. Build AD carries a touch
probe (input.c send_touch_event: TP: lines, DOWN/UP always, MOVE
thinned to >=6px, cap 1200) to record her scripted gestures:
open Angle -> slow drag right -> lift; open Power -> slow drag
left -> lift; tap Fire. From TP coords + her reported end values,
derive the mapping; the physical-button synthesis (Build AE)
injects records at this same layer (synthetic slot in touches[],
driven from poll_gamepad when the GAME scene is current).

Physical scheme (proposed): Cross = Fire tap; D-pad L/R = angle
down/up; D-pad U/D = power up/down — exact gesture shapes pending
the probe data.
