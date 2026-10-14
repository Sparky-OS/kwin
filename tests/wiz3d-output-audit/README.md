# wiz3D output audit

Audit tools for KWin baseline ec2519f5ec and the subsequent output-filter patches.
The report and evidence live in `/K3D/temp/partners/queue/kwin-wiz3d-outputs`.

`prepare.sh` copies this checkout's protocol XML and extracts installed +stereo3d21 package files from `waterfox-stereo-bench:m3-kwin21`.
`run-vm.sh` builds the probes in `kde-ci-local/suse-qt613:latest` and boots a headless VKMS guest inside a non-root, four-CPU Docker container.
One VKMS device provides two independent connectors; it exists only in the guest.
QEMU has no network, host GPU or graphical display.
KWin binaries are overlaid in guest RAM, and runtime hashes are recorded.

The local rig currently references the kernel/initramfs in the mesa-stereo lane and a read-only root in the displayxr-kwin lane, both reused during the original audit.
The milestone 2 brief expressly authorizes those two exact fixture paths for read-only reuse.
Other partner paths remain private.
The scripts do not authorize access to those folders by themselves.
The existing PNG evidence can be compared without accessing any external guest fixture.

## Reproduce captures with authorized fixtures

Run from the workspace after supplying those fixtures:

```sh
export DOCKER_HOST=unix:///K3D/temp/partners/queue/kwin-wiz3d-outputs/docker.sock
audit=/mnt/build/kwin-wiz3d-outputs/kwin/tests/wiz3d-output-audit
sh "$audit/prepare.sh"
printf '%s\n' matrix > rig/case-set
cp "$audit/run-vm.sh" rig/run-current.sh
sh rig/run-current.sh
```

Run only one guest at a time.
Repeat with `ized-first` or `sequential-fresh` in `rig/case-set` for the clean-start follow-ups; they write separate evidence folders.
Keep a running shell script's snapshot unchanged until it exits.
The original passing swapped-input pilot used `guest-session.sh` at commit 201d91a48c; the full matrix deliberately also exposes the failing iZ3D-to-interleave transition.
Do not substitute that failing transition for the passing pilot's negative control.

## Compare saved evidence

```sh
export DOCKER_HOST=unix:///K3D/temp/partners/queue/kwin-wiz3d-outputs/docker.sock
audit=/mnt/build/kwin-wiz3d-outputs/kwin/tests/wiz3d-output-audit
docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" --cpus 4 --network none \
    -v "$PWD:$PWD" -v "$audit:/audit:ro" -w "$PWD" \
    waterfox-stereo-bench:m3-kwin21 sh -eu -c '
    for folder in vkms pilot ized-first sequential-fresh; do
        python3 /audit/reference.py "evidence/$folder" > "evidence/$folder-comparison.log"
    done
    python3 /audit/check-results.py evidence
    '
```

`reference.py` accepts raw PPMs or lossless `*-framebuffer.png` replacements.
It writes cropped/transformed panel captures, independent expected images and exact comparison JSON.
Additional arguments are equality gates; for example `reference.py evidence/vkms rows-left` exits zero, while `reference.py evidence/pilot actual_swapped_input` must exit one.
The JSON's one-LSB diagnostic is not an equality tolerance.
The final assertion script checks 18 exact captures, their swapped references, a genuinely swapped input, and the retained anaglyph/iZ3D differences.
A passing assertion script means the reported evidence is internally consistent; it does not certify every method or sequential timing.

## What the probes observe

`eye-fixture.c` declares exactly one full-SBS surface, with a one-eye viewport and coordinate-dependent L/R colors.
It waits for three frame callbacks before reporting readiness in the current rig.
`make-control.py` uses the compositor's existing output-management API and waits for an applied/failed response.
`make-edid.py` gives the synthetic EDID a 640×480@120 native timing so interleave and sequential choices can be selected without physical screens.
`drm-grab.c` reads the primary framebuffer and its plane source/transform properties, retaining the high eight bits from AB48.
`drm-flipwatch.c` records plane source x once per vblank or at a chosen longer polling interval.
The reference models DX9 sRGB state as well as shader algebra, and distinguishes iZ3D MRT from multipass front output.
For DX10 UNORM input/output, the encoded-CF diagnostic is also the corresponding analytic formula.

## Credits

The initramfs assembly and client/readback starting points come from Daniel Ramos's Sparky Stereo OS `tools/vkms-stereo-bench` and its existing probe derivatives, under GPL-2.0-or-later.
QEMU, Linux VKMS, Mesa llvmpipe, Wayland and KDE KWin supply the execution path.
The reference targets wiz3D/effcol and the output-method sources credited to iZ3D Inc. in their 2002–2010 headers; `LICENSE-iZ3D.txt` names Bo3b Johnson.
The optimized anaglyph method is credited to Eric Dubois.
See the workspace's proposed attribution addition for the main session's public integration review.

## Shader cache regression

`cache-rows-first` performs rows → iZ3D → rows in one compositor session.
`cache-ized-first` performs iZ3D → rows → iZ3D in a separate fresh session.
Set `AUDIT_OUTPUT=m2/cache-before-rows` (or another distinct relative path) when invoking the runner to keep baseline and patched evidence separate.
The guest records hashes of the binaries it actually runs.
After the baseline runs, `prepare-packages.sh packages/cache` replaces the guest overlay with the locally built Debian packages; repeat both sequences under new evidence paths.
Run `check-cache.py evidence/m2/cache-before-rows rows-first`, or `ized-first` for the reverse sequence, in the Python/PIL container.
The check exits nonzero on a missing capture, any differing pixel, or a failed eye-order control.
CF's back average is mathematically invariant under exchanging eyes, so its swapped prediction must also match; each front and rows capture must reject the swapped prediction at every pixel.
The baseline must fail and the changed package must pass both complete sequences.

## Per-output eye exchange

`swap-matrix` starts a fresh compositor, D-Bus session and configuration directory for each output mode in one isolated VM.
It uses a second coordinate-tagged palette with unequal eye luminances, selected by `PALETTE_FIXTURE=1`.
Each mode is captured with exchange off and then on; each pair is also captured with only its first output exchanged before enabling the second output's exchange.
The control client requires the version-26 request and checks the returned setting event after applying it.
The extended synthetic EDID includes full side-by-side at 1920×1080; frame packing uses 1280×720 with the declared vertical interval.
`check-swap.py` checks every capture against the palette oracle, including preserved RightFirst modes, eye selection on 2D outputs, and unchanged ordinary 2D content.
For anaglyph exchange it models the existing KWin gamma-2.2 path, whose difference from wiz3D's piecewise-sRGB path is already recorded in milestone 1.
The checker requires exact equality, with no one-LSB tolerance.
This matrix does not certify shutter timing or change its scheduling.

The `2d` case enables a 2D output beside a rows output and maps a separate declared fixture on each named connector.
It captures both outputs with the 2D output's swap off and on.
The `2d-mono` case repeats this with undeclared 2D content on the first output, which must remain unchanged.
`FIXTURE_OUTPUT` selects the fixture connector; `MONO_FIXTURE` selects its ordinary 2D control.
Optional whitespace-separated names in `rig/swap-probes` restrict a run; pass the same names after the evidence folder to `check-swap.py` when checking a subset.
Retain the run's control, fixture and guest script snapshots beside its binary hashes.

`swap-settings` keeps one rows fixture mapped and changes its setting through the newly built libkscreen backend.
The initial, exchanged and restored captures test repainting without new client damage.
Build `swap-settings.cpp` against the candidate libkscreen, supply that library and its KWayland plugin in `rig/ui/`, then run the case with its own evidence folder.
