# wiz3D output audit

Audit tools for KWin baseline ec2519f5ec, with no compositor changes.
The report and evidence live in `/K3D/temp/partners/queue/kwin-wiz3d-outputs`.

`prepare.sh` copies this checkout's protocol XML and extracts installed +stereo3d21 package files from `waterfox-stereo-bench:m3-kwin21`.
`run-vm.sh` builds the probes in `kde-ci-local/suse-qt613:latest` and boots a headless VKMS guest inside a non-root, four-CPU Docker container.
One VKMS device provides two independent connectors; it exists only in the guest.
QEMU has no network, host GPU or graphical display.
KWin binaries are overlaid in guest RAM, and runtime hashes are recorded.

The local rig currently references the kernel/initramfs in the mesa-stereo lane and a read-only root in the displayxr-kwin lane, both reused during the original audit.
The updated house rules forbid reading other partners' workspaces, so a new run requires authorized shared fixtures or copies supplied by the main session; update `old` and `root` in `run-vm.sh` accordingly.
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
docker run --rm --user "$(id -u):$(id -g)" --cpus 4 --network none \
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
