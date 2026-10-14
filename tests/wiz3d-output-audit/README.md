# wiz3D output audit

Audit tools for KWin baseline ec2519f5ec. They do not change compositor code.
The report and captured evidence live in `/K3D/temp/partners/queue/kwin-wiz3d-outputs`.

`run-vm.sh` boots an isolated VKMS guest inside a non-root, four-CPU Docker
container. It mounts the existing kernel and guest root read-only and overrides
KWin files in guest memory with files from installed package +stereo3d21.
The two VKMS devices never exist on the host. QEMU has no network or display.

`eye-fixture.c` is a fullscreen Wayland client declaring exactly one full-SBS
surface. Left pixels have red in 128..191 and blue in 16..79. Right pixels have
those ranges reversed. Coordinates vary per pixel to expose sampling shifts,
reflections and ordering errors. `SWAP_EYES=1` reverses the actual input.

`make-control.py` generates an output-management client from this checkout's
protocol XML. It sets modes and pairs through the compositor's existing API.
The generated event handlers consume all advertised events; the probe waits for
an applied/failed response instead of assuming configuration succeeded.

`make-edid.py` replaces the public bench EDID's preferred timing with a small
640x480@120 mode (50.4 MHz, total 800x525). This exposes native-resolution
interleave modes and frame sequential modes without a physical screen.

`mkcpio.py` and the starting point for `eye-fixture.c` come from Sparky Stereo
OS's public `tools/vkms-stereo-bench` (GPL-2.0-or-later). The runtime reuses that
bench's configfs helper and DRM grabber. QEMU, Linux VKMS, Mesa llvmpipe, Wayland,
and KWin provide the execution and capture path. The comparison targets are
wiz3D by effcol and the iZ3D output-method sources, whose headers credit iZ3D Inc.
`LICENSE-iZ3D.txt` in wiz3D names Bo3b Johnson. No contacts are copied into the
public audit documentation.

A successful boot or black frame is not an output-equivalence result. See the
workspace report for measured comparisons and remaining gaps.
