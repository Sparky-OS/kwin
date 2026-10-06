# Stereo 3D in KWin: the design in one page

This branch adds stereo 3D output to KWin.
This page is the design behind it, written for the review in [plasma/kwin#324](https://invent.kde.org/plasma/kwin/-/work_items/324).
It is not part of the merge requests.

## One format inside

**KWin talks full side by side:** both eyes at full size, left eye first, at any resolution.
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
**Anaglyph:** the same Dubois matrices (modern screens and CRT, in linear light) were tested in a game, Half-Life 2 handing its full side by side eyes to a gamescope effect, on the HX855 (2026-09-25, both profiles). In KWin it is built and switched by an environment variable for now; its first screen test comes with the toggle.
**Planned, in this order:** the virtual 3D modes and their toggles, the other stereo formats, VR.

Daniel (Sparky Stereo OS, an edition of SparkyLinux)
