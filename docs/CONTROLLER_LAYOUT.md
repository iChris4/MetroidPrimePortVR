# Custom VR controller layout

Open the Controller tab and choose **Customize controller layout**. Pick a controller
button-label family, then choose an input beside each action. **Apply** updates the
launcher; **Save Settings** or **Play** persists it. Cancel discards changes. Disable
**Use custom controller layout** to return to the standard controls.

When enabled, custom layout controls VR input in PrimedGun mode and takes priority
over standard VR bindings, desktop keyboard/gamepad remaps, the A-to-jump toggle,
and the alternate A/B control scheme. Handedness still chooses the tracked weapon;
movement speed, turn sensitivity and rumble settings still apply. The movement/look
stick selections in the editor replace the standard stick assignment.

- **Jump:** keep the main binding on a stick direction and assign an additional
  button, or change either binding independently.
- **Beam select:** hold its assigned input, swing the weapon controller toward the
  desired beam, then release. It is a hold modifier, not a next-beam button.
- **Visors:** choose the near-head gesture, hold a selector input while using the
  visor stick, or use only physical D-pad / directly assigned visor inputs. The
  selector's stick does not move, turn or jump while it is changing visors.
- **Steam Frame:** select its controller type to translate standard bindings and use the native D-pad
  for Combat (up), Thermal (down), Scan (left) and X-ray (right). Its right X/Y
  buttons and shoulders are separate assignable inputs. The native profile is
  used when the runtime advertises XR_VALVE_frame_controller_interaction.
- **Vive / WMR:** their presets use triggers, grips, menu buttons and pad clicks
  instead of unavailable face buttons. Wands leave the separate height binding
  unassigned; choose an available input if you want it.
- **Morph Ball and menus:** have their own action bindings. On-foot jump does not
  change bomb, boost or unmorph bindings.

Only inputs present on the controller and exposed by its OpenXR runtime can be
used. Headset system buttons may be reserved by the runtime. Existing Touch,
Index, Vive, WMR, native Pico and simple-controller profile bindings remain available; other
controllers can use the compatible profile their runtime provides. No hardware
can supply a missing physical control. Controller button-label selection in the
editor changes the labels/preset, not the runtime's device selection.

The PC and Quest launchers use the same persisted vr_custom_* keys. Binding IDs
and defaults are shared through platform/include/vr/vr_controller_bindings.inc.
Custom layout is off for existing and new installations until enabled explicitly.

## Input sensitivity

The **Input sensitivity** tab adjusts trigger travel, grip/squeeze pressure,
touchpad pressure and stick-direction activation from 5% to 95% (default 55%).
Lower values activate sooner. The stick slider controls stick-up jump and other
bound stick directions; movement and turning keep their own deadzones. Additional
button jump is independent. Pressure thresholds require an analog input exposed
by the controller/runtime; digital-only triggers, grips and touchpads use clicks.
Changes apply with the layout and Cancel discards them. Load defaults resets all
four sliders. These settings affect custom bindings, including VR menu controls.

Changing controller type translates bindings that match the previous controller
defaults to the new controller equivalents. Custom assignments and sensitivity
values are retained. Load default layout resets every binding to the selected
controller preset and restores sensitivity defaults.

## Visor wristband

Enable **Controller > Visor selection > Off-hand visor wristband** (off by default).
It also appears in the F1 VR settings and the headset's Port Config page.
The HUD's actual visor icon and frame models follow the off-hand wrist in a single row on the side of the forearm
(mirrored for left-handed play).
Bring the main controller above an icon (up to 12 cm) and select it
(A or right trigger in right-hand mode; X or left trigger in left-hand mode). The wrist calls the same model draw routine as the HUD, using the actual widget
colour, inherited opacity, HUD draw alpha, lighting and authored blend mode. Hover enlarges the icon slightly; locked visors remain dim. They draw after the visor effects, alongside the HUD, while retaining tracked
world-space placement and drawing over the gun. Unacquired visors are dim and cannot be selected.
The symbols draw over the cannon; their transparent areas reveal the scene.

The wristband works with the original controls and custom layouts. Selection uses a stable hover and buffers the press until a simulation tick
can accept it (up to 0.65 seconds). It requires a fresh press and consumes that
button until release, preventing an
accidental shot, jump or other bound action. It is hidden in menus, cinematics and
Morph Ball, and cannot change visors during scanning or grappling; a press during a short visor transition is buffered.

### Wrist placement

The approved placement defaults are X/Y/Z = 0 m, pitch 40°, yaw -40°, roll 50°.
Calibration controls have been removed from the menus. Saved placement values
remain supported, and controller/config reset restores these approved defaults.
The whole row uses one rigid transform, keeping icon faces parallel.

The row is ordered **X-ray, Thermal, Scan, Combat** from left to right. Icon
symbols rotate 90 degrees left within their frames, reversing the quarter-turn
for left-handed play so they stay upright. Blend passes match the HUD exactly;
there is no independent wrist brightness boost or forced full opacity.
