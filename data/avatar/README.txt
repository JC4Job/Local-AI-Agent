The avatars of the agents
=========================

agent_gui.exe draws each agent's avatar beside its conversation. An agent's avatar is the folder with its name
(as its life file in data\life\ is named), and everything about it is in that folder, one file per kind of
information:

    data\avatar\<agent name>\
        model.txt          the model: the .obj to show, its axes, how its materials shine     (needed)
        camera.txt         how the camera sees it: target, distance, height, fov
        lights.txt         its lights and background, what an animation does not set
        rig.txt            its bones: what the animations move
        anims\<key>.anim   its animations, one per life state

    data\avatar\Sof_IA\    the default agent (agent_name of config\settings.txt)
    data\avatar\Plato\     the agent of config\agents\Plato.txt

An agent with no folder here shows where to make one. Right-click the avatar > Open the avatar folder makes it
and opens it. Every file but model.txt is optional: what one leaves out is worked out (the camera) or the
default (the lights, a rig of one bone). Each file starts with comments that explain its keys.

The window reads a folder again within a second of a change, so you can edit these files with it open.
Problems with a file (an unknown key, a bad number) show under the avatar; hover there to read them. A key put in
the wrong file says which file it goes in.

bin\avatar_render.exe draws an avatar to a .bmp without the window: avatar_render --agent Plato out.bmp. It
prints the camera it worked out; --parts lists the model's parts, to write rig.txt.

Format of every file: one "key = value" per line. Lines starting with # are comments. Numbers use a decimal
point.


Animations: anims\<key>.anim
============================

agent_gui.exe plays the file of the agent's life state, and blends into the next file when the state changes:

    homeostasis.anim       Homeostasis      balanced
    fight_or_flight.anim   Fight or flight  acute stress
    chronic_burnout.anim   Chronic burnout  depleted
    deep_rest.anim         Deep rest        recovering
    high_motivation.anim   High motivation  driven
    lethargic.anim         Lethargic        low energy
    default.anim           any state that has no file of its own (without it, such a state stands still)

A file with another name (talking.anim) is not played by itself, but right-click > Preview shows it, ready for
a state to come. Right-click > Import animation for copies a file in for a state, into the anims\ folder of that
agent's avatar (the one it replaces goes to config\backup\).

blend = <seconds>
    How long the blend into this state takes. Default 0.6.

Lights and background, all optional: what a file leaves out comes from the avatar's lights.txt.
    key_light, fill_light, rim_light = <r g b>    multiples of white light; above 1 is brighter
    key_light_dir, fill_light_dir, rim_light_dir = <x y z>    toward the light
    ambient = <r g b>                              light from everywhere, brighter from above
    background_top, background_bottom = <r g b>    sRGB colors, 0 to 1

track = <bones> <channel> <curve>
    bones    a bone of rig.txt (head, eye.L...), or a name ending in * for every bone starting so:
             eye* moves eye.L and eye.R together. A bone the rig does not have is skipped, with a warning.
    channel  rx ry rz   turn, in degrees: rx > 0 nods down, ry > 0 turns to the character's left,
                        rz > 0 raises its left side (tilts the head toward its right shoulder)
             tx ty tz   move, in model units (Sofia's head is about 2 tall, Plato about 150): x to the
                        character's left, y up, z forward
             sx sy sz   scale along x, y, z: 1 is as modeled, 0.5 half
             s          scale along all three
    curve    hold <value>
                 that value, all the time
             wave <amplitude> <seconds> [phase] [center]
                 a smooth swing around center, one cycle in that many seconds. phase (0 to 1) shifts it
                 along its cycle; center is 0 for turns and moves and 1 for scales unless given.
             keys <seconds> [vary <share>] <t> <v> <t> <v> ...
                 values at times (t, in seconds from 0 to <seconds>) of a loop that repeats, eased in and out
                 between them. vary (0 to 0.9) makes each loop up to that share longer or shorter at random,
                 so a blink does not come like a clock. Tracks with the same seconds and vary stay in step.

Tracks on the same channel of a bone add up; for scales, their differences from 1 add up. A bone's children
follow it: turning head turns the eyes, brows and lips with it.

Example: a breath every 4 seconds and a blink about every 4.5.

    track = body s  wave 0.006 4
    track = eye* sy keys 4.5 vary 0.3  0 1  4.05 1  4.2 0.1  4.35 1
