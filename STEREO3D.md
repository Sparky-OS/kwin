# Stereo 3D in KWin: the design in one page

This branch adds stereo 3D output to KWin.
This page is the design behind it, written for the review in [plasma/kwin#324](https://invent.kde.org/plasma/kwin/-/work_items/324).
It is not part of the merge requests.

## The standard in three lines

- **Stereo KWin speaks full side by side, at any resolution.**
- **Programs** decode or generate stereo in any standard, and deliver to KWin full side by side, at any resolution.
- **Outputs** convert from KWin's full side by side to whatever each screen needs.

That is what makes stereo work across GPUs, across monitors (2D ones included) and across every 3D output format.

**Two places, two rules.**
Inside a program, anything goes: a player reads every packing a file can carry, an engine renders its eyes however it likes.
Inside Stereo KWin, only full side by side exists: both eyes at full size, left eye first, at any resolution, declared once.
The output filters then turn that into whatever the screen needs: a 3D television's own HDMI 3D modes, anaglyph on any monitor, interleaved and frame-sequential displays, a headset.

**A window's 2D part and its 3D area are separate surfaces.**
The 2D part (menus, toolbars, panels, the document around a picture) is the window's own surface, at the window's normal size, one eye's size, and declares nothing.
The 3D area (a stereo video, a 3D viewport, a stereo painting canvas) is a surface of its own, a subsurface on Wayland, that declares full side by side.
Stereo KWin shows the 2D part identical in both eyes and gives the 3D area each eye's view, in place inside the window.
Previews of stereo content are 3D areas too: a file manager's thumbnail of a stereo photo or video, or its preview panel, gives each eye its own view by default, beside names and icons that stay identical in both eyes.
On a stereo desktop a flat preview of a stereo file is the question every newcomer asks ("isn't this a stereo desktop?"), so 3D is the default and flat is the user's choice.
A program that makes its whole window stereo while it has a 2D interface gets that interface doubled with the picture and laid out across both halves, squeezed to half width in each eye; in Qt, that is stereo set on the application's default surface format instead of on the 3D area's own window (`QWidget::createWindowContainer` around a stereo `QWindow`).
The one exception is a program that is stereo and full screen only: its whole window is the 3D area, so its whole window is stereo.
The separation is also what gives the 2D part its depth: its elements declare their planes (sunk, screen, popped), and Stereo KWin draws those for each screen, while the 3D areas carry their own eyes.

**It is how sound already works on Linux.**
A program hands PipeWire plain PCM, at whatever rate it likes.
PipeWire resamples, mixes and sends it to a headphone jack, HDMI, Bluetooth or a USB DAC, and the program never learns which.
Nobody calls that a limitation: it is why every Linux program plays sound on every device.
Full side by side is the stereo picture's PCM, and Stereo KWin is its PipeWire: one plain format in the middle, freedom on both sides of it.
A new kind of 3D screen needs one new output filter, and every stereo program already written works on it the day the filter lands.

## One format inside

**KWin talks full side by side:** the two eyes as separate pictures next to each other, left eye first, each at the resolution it has.
It is not a new format.
It is HDMI 1.4's 3D structure 3 ("side by side full"), and the layout SteamVR writes its stereo screenshots in.

**Programs hand over that, and declare it once.**
- **Games:** a VR engine already renders its two eyes side by side.
- **Video players:** they unpack whatever the file carries (half side by side, top and bottom, frame packing, MVC, either eye first) and hand over full side by side.
- **Photo viewers:** MPO and JPS become the same pair.
- **The declaration** is an X11 window property or a Wayland protocol, plus a window rule for programs that cannot declare. It carries one value: full side by side, left first.

**KWin never needs to know a source format.**
Each specification stays where it belongs: HDMI in the drivers, the H.264 SEI and Matroska's StereoMode in the players, the display in KWin.

## Why one format, and why full side by side

**Every format the inside accepts multiplies the work of everyone who wants stereo.**
With several formats in, the cost is every input format times every output format times every program that wants stereo.
Input formats and output modes are short lists, owned by the compositor's authors.
The programs are an open list, owned by everyone else, and that is the term that never ends.
Every earlier attempt put the format question on each program: quad-buffer needed a workstation card, 3D Vision needed one vendor's driver, and every player grew its own menu of packings.
Each program had to know the display, and almost none did.
With one format, a program learns stereo once: it renders two eyes, declares them, and never learns what a television or a headset is.

**Full side by side, at any resolution, is that one format for four reasons.**
- **Programs already produce it.** Games and VR engines render two viewports side by side, most 3D files carry side by side, and Mesa packs quad-buffer into it at swap. The fewest programs have to change.
- **It loses nothing.** Each eye keeps its full resolution, so no packing chosen today limits a better display tomorrow. Half formats exist only at the edges, where a display asks for them.
- **It survives the path.** One ordinary buffer passes through X11, Xwayland, screenshots, screencasts and remote desktop unchanged.
- **The eyes stay in sync.** Both eyes travel in one buffer, committed once, with one presentation time, so they always show the same instant and can never drift apart. That is what lets every output be built from it: frame packing, interleaving, anaglyph and frame sequential all take their two views from the same frame. Two buffers per frame would need their own synchronisation, at every step and in every program.

**The cost, stated plainly.**
A fullscreen video already in the display's own mode is unpacked by the player and packed again by the compositor.
That conversion is copying and scaling rows and columns, work a compositor does every frame anyway, and every test of this branch runs in software rendering as well as on the GPU.
Where it matters, the compositor can skip it internally: an optimisation inside one program, not a format question pushed onto every program.

## Every output is a filter at the end

**One packer, at the very end.**
On the first game run through this branch, the game packed top and bottom for the TV and KWin packed again: the whole packed frame went into each eye.
With one packer at the end, that cannot happen.
And instead of an adapter for every source and every kind of gear, each source is written once and each kind of gear once.

**The display's own 3D modes** (HDMI 1.4 3D, from the EDID) are listed with the 2D modes, labelled, and never chosen automatically.
Choosing one sends the InfoFrame, and the display switches itself.

**Virtual 3D modes** are twins of every 2D mode, with the same timing and the same link: no InfoFrame, no modeset.
KWin renders both eyes at the mode's full resolution and composes them into the 2D frame.
Two toggles per output, both off by default, so the mode list stays as it is until someone asks:
- **Anaglyph:** red/cyan for modern screens and for CRTs (Dubois matrices, in linear light).
- **Other stereo formats:**
  - side by side and top and bottom (half), for a 3D display switched by hand;
  - rows, for passive polarised panels;
  - columns, for parallax-barrier and lenticular panels;
  - checkerboard, for 3D-ready DLP televisions;
  - frame sequential at 100 or 120 Hz, for gear with its own glasses sync;
  - two outputs as one pair, for dual projectors, mirror rigs and dual-panel monitors.

**VR is one more filter:** each eye goes to the headset through OpenXR, instead of through a colour filter.

Each row of the table "Displays decide the format" in [awesome-stereoscopy](https://github.com/danielcamposramos/awesome-stereoscopy#displays-decide-the-format) is one filter.

## An output that replicates another one copies, it does not paint again

**The scene is composed once, as the two eyes, and every output on the way out is a filter.**
When an output replicates one with stereo eyes (a 3D television beside an anaglyph panel, a projector copying a 3D monitor, a 2D screen copying the left eye), the output that has the logical output composes the scene, and each replica takes what it composed:
- the composing output keeps the two eyes of each frame (the buffers it painted the scene into, one picture per eye, at its own pixels) while any replica asks for them, and gives them up when none does;
- a replica copies each eye into the eye buffers it would have painted the scene into, scaled once into the place its view shows the scene (black around a mirror that does not fill the output), and applies only its own layout: frame packing, anaglyph, half side by side, rows, columns, checkerboard, or the left eye for a 2D screen;
- the copy is a blit: the replica makes no scene pass, so a mirror costs one scene pass per eye in all, not one per output and eye;
- the composing output wakes its replicas when it has new eyes; a replica that finds no eyes yet (the first frame of the mirror) paints the scene once as KWin always did;
- the eyes' buffers stay out of the swapchain's reuse while the pair is held, and a replica adds its fence to their release points after its copy.

What a replica needs from the eyes, else it paints the scene itself as before: the same GPU, an unrotated output, no ICC profile, and a layout that takes its eyes from the shadow buffers (not frame sequential, not a stereo pair of two outputs, not the picture and depth layouts, which are composed differently).
The source needs stereo eyes too; a 2D source with a stereo replica keeps painting the scene for the replica (the replica needs both eyes, the source composes one).
Where the replica copies, the pointer and the overlays are already in the eyes, so the replica uses its primary plane only, as an output with stereo eyes does.
`KWIN_STEREO_REPLICA_COPY=0` makes every output paint the scene itself, as before; a mirror of two 2D outputs never copies and is unchanged either way.

**Another colour space is converted once, on the copy.** Outputs that blend in different colour spaces (brightness or dimming, HDR on one and not the other, another colour resolution) are the common case of a laptop panel and a TV. The replica then draws the eyes into its own with one conversion, scaled at the same time (one draw instead of the blit), with a conversion table built once for the pair of descriptions from KWin's colour pipeline (`ColorConversionTable`, `src/core/colorconversion.cpp`), cached, and reused for every frame, both eyes and every output; it is rebuilt only when either description changes. The table is the same one `GLShader::setColorspaceUniforms` uses for the scene pass, and the one that tone-maps HDR content (PQ, BT.2020) into an SDR output with deep colour, in floating point until it is encoded once at the output's depth. The copy is used when it gives what painting the scene again would give, within one level of 10 bits (so of 8) for SDR content, measured on grey steps, the primaries and mixed colours with the pipeline itself; HDR content on screen leaves the copy on the old path when the tone mapping of the converted eyes differs from painting again, and an ICC profile on the replica does too. The reason is logged ("cannot copy the eyes: another colour space: ..."). `KWIN_STEREO_REPLICA_COPY=0` keeps the old path everywhere.

## Every HDMI 3D structure, picture and depth included

**The display's 3D modes are every structure of HDMI 1.4a** (Appendix H, Table H-2): frame packing, field alternative, line alternative, side by side (full and half), top and bottom, and the two picture and depth structures.
Each mode is the frame the kernel times for it: field alternative and L + depth like frame packing (the second picture from line vtotal on), L + depth + graphics + graphics-depth four pictures one vtotal apart, line alternative the eyes line by line, left first.

**Side by side (full) has two timings, a choice per output.**
The whole line doubled, as Figure H-4 does, is the default; the horizontal blanking kept, as NVIDIA's driver does, is the other value, for a display that refuses the first.
The picture is the same; only the signal differs.

**A picture and depth display makes the views itself.**
In L + depth (Figure H-5) KWin sends the left eye's picture and its depth, and the display renders each view from them.
What the depth shows is a choice per output:
- **Flat:** everything at the screen.
- **Desktop:** the depth levels above, exact, each window at its level and 3D content at the level of its window.
- **Desktop and 3D content** (the default, since 3D is why such a display is there): the same, and the depth of 3D content estimated from its two views, by matching blocks along the eyes' axis on the GPU at a quarter of the size; where the views match at more than one parallax (a plain area, a pattern that repeats along the axis) they don't tell the depth, and the content keeps its window's level there; where an effect moves windows, the whole picture is estimated.

**With graphics, the chrome is the graphics.**
L + depth + graphics + graphics-depth (Figure H-6) sends a second picture with its own depth, for the display to lay over the first.
KWin puts the desktop's own chrome there (panels, menus, popups, notifications, on-screen displays and the pointer) at its depth level, and the windows in the picture.
How a display lays the graphics over the picture is not in the published extraction of the specification, so this structure is drawn by the specification and marked untested on a display.

**The depth is parallax, and the 3D metadata says how much.**
Each depth sample is the parallax between the eyes: 128 is the screen, larger is behind it.
The 3D metadata of the vendor InfoFrame (Table H-4) carries the parallax information of ISO/IEC 23002-3: parallax_zero, parallax_scale, the viewing distance (dref, a setting per output, three picture heights by default) and the screen's width (wref, from the EDID).
The formula behind it is our reading of sections 6.1.2.2 and 6.2.2.2 of ISO/IEC 23002-3, unverified against the standard, whose text is not freely available.
With the eyes swapped, the picture is the right eye's and the parallax runs the other way.

## Depth on the desktop

**Windows and elements get real depth instead of clever trickery in 2D.** Desktops have long drawn depth with 2D tricks: a bigger shadow for the focused window, dimmed inactive windows, menus and tooltips lifted by their shadows, dialogs blurring what lies behind them. The meanings stay the same here (higher is more important, lifted is active, behind is waiting); only the cue becomes real: two eyes and a real distance.

**Three places, declared: sunk, screen and popped.** All subtle, a few pixels, with the limits the user's own settings (System Settings, "3D Depth": how far popped comes out and sunk goes back, in pixels at 1920 wide, scaled to each screen). The active window sits at the screen; its menus, tooltips and notifications pop; the wallpaper and the windows behind sink.

**Everything in between is KWin's job, by stacking order.** Several stacked background windows fan out from sunk up through the screen towards popped, always behind the active window, in the same order as in 2D. A window caught in the middle of a pile, its edges out of the stack, then reads as a sheet floating within it, the way a pile of papers looks in real life. No program declares this; it falls out of the order.

**A change of level is a short motion, and depth follows the stack.** A window or the pointer that moves to another level gets there in a short motion that eases out: towards the viewer in about 80 ms, so that nothing sits behind what it is drawn over for more than an instant, and away in about 150 ms; both follow Plasma's animation speed and are instant when animations are off. The active window comes to the screen when it is on top of what it overlaps, so with focus following the mouse and a delayed raise the active colours come just ahead of the pop, and System Settings keeps the focus delay a little shorter than the raise delay (700 ms for 750 ms) unless the person sets their own. A popup is never behind what it was opened from: a sub-menu that ends up under its menu shares the menu's level.

**In the background, each window is 2D.** A window in the pile is a flat sheet at its place in the stack: the desktop's pop effects inside it (its popped elements) come to one plane, which is exactly what makes the pile read as a pile of papers. Its stereo content stays 3D: a movie or a game is full side by side whatever its window's place, so a 3D movie keeps playing in 3D on one screen while work goes on in another. 3D inside 2D holds everywhere.

**Pop is real, not faked: a little scale and a little depth, together.** One value per element gives both cues, by one formula: bigger and nearer when popped, smaller and further when sunk.
- **Everything follows its element's plane:** its text scales and moves with it, and so does its click area, so a popped element has no dead margin and a sunk one no click area beyond its picture. The plane's geometry is an instance inside the window itself, the one geometry that both drawing and input read, so the click area follows by construction.
- **Text shares its element's depth.** What tires the eyes is 2D text lying over a picture at another depth, not depth as such.
- **In 2D, the pop is the same scale plus a move up and to the left;** in stereo the move is the eyes' disparity instead. A straight 2D screen shows the left view, and the left eye's own shift would move a popped element the wrong way, so there the up-left move replaces the eye shift.

**Depth as meaning, measured.** Diego González-Zúñiga's doctoral thesis, [Stereoscopic User Interfaces: Creating a Pipeline for Stereo Application Development](https://ddd.uab.cat/record/167810) (Universitat Autònoma de Barcelona, 2016), added depth to ordinary interfaces "to imply importance, rating and hierarchy": nearer for what matters, depth for what is selected, a plane for each group. His measurements back this design. Separating groups by depth made people faster at a memory task. Stereo raised dwell and fixation time and changed the order in which people looked at a page ([Breaking the Pattern: Study on Stereoscopic Web Perception](https://doi.org/10.1007/978-3-319-03176-7_4), 2013), and people chose words like "easy to use" and "clear" for the 2D versions and "attractive" and "fun" for the stereo ones. That is why the active window, where the reading happens, stays at the screen, and depth is spent on everything around it. His tools made one application at a time stereo; here the compositor does it once, for every program.

**Effects speak depth too, with their own limits.** An effect describes its motion once, in three dimensions: a vertex of its quads carries a depth in pixels of a view 1920 wide, the unit of the limits above. The renderer's projection is where the two cameras are: on a stereo output it shears depth into the offset between the eyes and drops it, so no effect asks which eye is drawn, and on a 2D output nothing changes. Effects have their own limits (`EffectSunkLimit` and `EffectPoppedLimit` in `[StereoDepth]`, on the same settings page), because without that freedom the creativity would be stuck: a closing window may reach further than where windows sit. One rule still holds for everyone: nothing is behind what it covers. Fall Apart is the first effect: "Fly" brings the pieces towards the viewer and "Dive" lets them fall to the nearest thing beneath them, hit it and bounce while they fade.

**Faked 3D becomes stereo by drawing it twice, not with cameras.** A scene that already fakes depth with size, rotation and order keeps its own drawing and draws it once for each eye, full side by side: each item moves by half of its parallax from the depth it already fakes, and a turned item does so per corner, through its own rotation seen by each eye (`StereoRotation`, Qt Quick's Rotation with an eye). The window switchers do this: in the Flip Switch the current window is popped and the others step back to sunk, in the Cover Switch the current cover is at the screen and the row recedes to sunk, both within the effects' own limits. On a 2D screen nothing changes.

## 2D screens beside stereo ones

With mixed screens, stereo is the king and a 2D screen is a place to see or to control.
Each 2D screen shows one of two things, chosen per output:
- **Anaglyph:** the full anaglyph (the whole screen colour-coded, a virtual 3D monitor fed the same full side by side as a 3D television), or the anaglyph only on 3D content (the 2D layer in normal colour, anaglyph only where stereo content is).
- **Straight 2D:** no stereo output of any kind; only the left eye is shown.

In straight 2D the second eye is never rendered: KWin draws one view, the left one, and nothing else.

## The Desktop Cube: a 3D application, the seed of the VR home

The cube is not part of the desktop's depth; it is a 3D application, treated like a game.
- **Its scene has its own depth**, its own eye separation and screen-plane distance, like a game's stereo settings.
- **It delivers what every program delivers:** full side by side, at any resolution, two Qt Quick 3D views of one scene, one camera per eye.
- **Its faces are the real desktops, each a stereo surface,** linked to the standard desktop renderer, not copied. The windows on a face sit on the desktop's three planes (sunk, screen and popped, with the pop the user's setting), so each face is a relief of three known layers, and the cube's two cameras see the windows standing off the faces from any angle, with no extra perspective handling.
- **When the cube closes on one desktop, it is the desktop:** the front face lands on the screen plane, identical to the flat desktop.
- **It is the seed of the VR home:** the same scene, with a headset's cameras in place of the cube's, is the floating 3D desktop on a headset output.

**In 2D, too.** On a straight 2D screen the cube renders only its left view, like everything else, but during the cube's animation nothing stops that view from being the real 3D scene: live desktops on the faces of a turning cube, the windows in relief, depth shown by perspective and motion alone. Desktop interfaces have seen little new in years; the same cube that is real depth on a 3D screen is something new on a 2D one, before anyone puts on glasses.

## What it was built against

**VR gear, shipped and shipping.**
Every headset runtime takes one image per eye: OpenXR (Monado, SteamVR) and OpenVR.
From the Valve Index to the Steam Frame and the Meta Quest line, that pair is what KWin already holds.

**3D gear from before.**
3D televisions and projectors over HDMI 1.4, passive polarised monitors, DLP 3D televisions, shutter-glass projectors, dual-projector rigs and dual-panel monitors.
iZ3D's output methods, carried on in [wiz3D](https://github.com/effcol/wiz3D), are the reference for these patterns.

**Glasses-free displays, current and coming.**
Parallax-barrier and lenticular panels take columns.
Eye-tracked lenticular monitors, such as Samsung's Odyssey 3D, steer two views to the viewer's eyes.
Light-field panels show many views, and two real views are a better start for them than one.
Filters for the eye-tracked and light-field kinds are future work; the two views they start from are already there.

## Where it stands (2026-10-02)

Tested on real hardware, with the kernel side in place, on a Sony KDL-46HX855 (amdgpu) and a KDL-46EX725 (RTX 3060, on NVIDIA's open modules and on nouveau):
- **the display 3D modes**, chosen in the display settings, with the flat desktop in both eyes: both TVs;
- **stereo windows** declared by a window rule, either eye first: the HX855;
- **a game handing over full side by side** (Half-Life 2 through gamescope), shown in 2D, side by side, top and bottom and frame packing: the HX855;
- **deep colour**, with new outputs preferring colour accuracy: 12 bpc, the TVs' maximum, on both.

**On this branch, tested in containers (2026-10-03), not yet on a screen:** programs declaring their stereo themselves, through the X11 property `_KDE_NET_WM_STEREO_CONTENT` with its class companion and through the Wayland protocol `kde-stereo-content-v1` (layout, class and sub-class, double-buffered, synchronized subsurfaces); a shared client library for both, in our plasma-wayland-protocols branch; and area-filtered downscaling for stereo windows shrunk more than 2:1, checked pixel by pixel against reference averages.
**X11 child windows, tested in containers (2026-10-16):** the 3D view of a program whose interface is 2D, Netgen's or gmsh's, is often a child window of its X11 window.
Mesa lists a stereo child in its window's `_KDE_NET_WM_STEREO_CONTENT_CHILDREN`, KWin redirects it so Xwayland gives it a surface of its own, and KWin shows that surface one view per eye at the child's place, while the rest of the window is the same in both eyes.
KWin announces this as version 5 of `_KDE_NET_WM_STEREO_CONTENT_SUPPORTED`.
**X11 views at their own resolution, tested in containers (2026-10-22):** an X11 window's buffer is its own pixmap, so on X11 the size a view is shown at is declared apart from its resolution, as a Wayland program sets its `wp_viewport` destination.
A program lists it in `_KDE_NET_WM_STEREO_CONTENT_ONE_VIEW_SIZE` (CARDINAL/32, the width and the height one view is shown at, in display pixels): the two 1920x540 views of a half top-and-bottom 1920x1080 video make a 3840x540 X11 window whose place on screen is one view at 1920x1080.
The X11 window keeps the program's own size, KWin resizes each view once, to the window's place, and the pointer reaches the first view at that place; a new declared size resizes the place, and a program chooses a new declared size when its place changes.
KWin announces this as version 6 of `_KDE_NET_WM_STEREO_CONTENT_SUPPORTED`.
**Every shrink is integrated (2026-10-22):** a view drawn smaller than its resolution, on either axis, is the average of the samples each pixel covers, taken within its own view; a view drawn at its size or larger is bilinear.
**Anaglyph:** the same Dubois matrices (modern screens and CRT, in linear light) were tested in a game, Half-Life 2 handing its full side by side eyes to a gamescope effect, on the HX855 (2026-09-25, both profiles). In KWin it is built and switched by an environment variable for now; its first screen test comes with the toggle.
**Planned, in this order:** the virtual 3D modes and their toggles, the other stereo formats, VR.

Daniel (Sparky Stereo OS, an edition of SparkyLinux)

## X11 one-view place requests (version 7)

A window with a valid ONE_VIEW_SIZE receives a proposed place when it is resized, maximized, tiled or made fullscreen.
KWin does not change the current destination until the client answers and submits its next frame.
The native X11 pixmap and ConfigureNotify geometry remain native geometry.

KWin writes the window property `_KDE_NET_WM_STEREO_CONTENT_ONE_VIEW_CONFIGURE`, CARDINAL/32, exactly three values: nonzero serial, width, height.
The dimensions are one view's display pixels, in the same units as ONE_VIEW_SIZE, from 1 to 32767.
A newer serial supersedes an earlier request.
The client selects PropertyChangeMask, chooses its buffer, paints, writes ONE_VIEW_SIZE with its chosen destination, and sends `_KDE_NET_WM_STEREO_CONTENT_ONE_VIEW_COMMIT` to the root with SubstructureRedirectMask | SubstructureNotifyMask.
That ClientMessage has format 32, its window is the client, data[0] is the request serial, and data[1..4] are zero.
The chosen destination may differ from the proposal.
Only the latest serial is accepted.
KWin holds Xwayland surface commits until the answer, then applies the answer with the next surface commit.
Removing the declaration or ONE_VIEW_SIZE cancels the pending request.
An unanswered request leaves the old picture at its old size.

The root `_KDE_NET_WM_STEREO_CONTENT_SUPPORTED` is 7.
libstereo-declare 1.6.0 exposes `stereo_x11_one_view_event()` and `stereo_declare_x11_one_view_commit()`; its API.md specifies validation and client use.
WM_NORMAL_HINTS dimensions and aspect bounds use one-view display units.
An explicit strict-geometry window rule remains a user choice.
Fullscreen proposes the whole output so the program can pad its native canvas.

ICCCM 4.1.5 requires refused ConfigureRequests to report the “(unchanged) geometry of the window”.
ICCCM 4.2.8 requires a ResizeRedirect recipient to use “the width and height specified”.
These mechanisms describe native X geometry, so neither carries a separate destination.
ICCCM 4.1.2.3 defines aspect fractions with “the numerator first and the denominator second”; an explicit base size is subtracted for aspect constraints, and a minimum size is not its substitute.
See [ICCCM](https://xorg.freedesktop.org/archive/current/doc/xorg-docs/icccm/icccm.html).
EWMH 6.2 says synchronization covers “the client repainting the window”.
KWin retains that repaint counter protocol and its existing timeout handling; the place acknowledgement is separate, and both gates must release a pending frame.
See [EWMH 6.2](https://specifications.freedesktop.org/wm/latest/ar01s06.html).
