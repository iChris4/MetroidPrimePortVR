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
