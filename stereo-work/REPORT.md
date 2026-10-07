STATUS: working

# Mesa stereo report — 2026-10-04

Partner: GPT-5 via Codex, branch partner/mesa-stereo.

Milestone 1 remains complete in commit 8a45d8f4dc1. Milestone 2 is complete
for the GLX DRI2/DRI3 and swrast paths. No class or subclass is declared.

Implementation:

- Gallium DRI creates stereo variants for double-buffered configs and
  exposes right-eye attachments when stereoMode is set.
- GLX reads _KDE_NET_WM_STEREO_CONTENT_SUPPORTED on the X11 root. Version 2
  or newer enables stereo on double-buffered configs; an absent or older
  property leaves the normal mono configs unchanged.
- DRI2 and swrast allocate separate left and right eye resources and pack
  them into one full side-by-side target at swap. DRI2 now keeps both eye
  resources while an application switches GL_BACK_LEFT and GL_BACK_RIGHT.
- Direct and swrast GLX resize a stereo window to twice its logical width and
  set _KDE_NET_WM_STEREO_CONTENT to CARDINAL value 3. No class or subclass is
  set.
- glXQueryDrawable reports the per-eye width for a stereo window. A viewport
  explicitly set to the doubled X width is mapped to the per-eye framebuffer.
- tests/glx-stereo.c checks compositor gating, GL_STEREO, per-eye drawable
  dimensions, independent eye reads, resize, declaration, and packed swap.
  The test accepts Xwayland's DRI3 BadMatch for XGetImage after swap; the
  packed-pixel check remains active where the X server exposes the window
  image, including llvmpipe under Xvfb.

Verification, all in Docker with DOCKER_HOST set to the workspace proxy:

- Source-built llvmpipe passed:
  `PASS: GLX stereo config, independent eye reads and packed swap` in
  `evidence/devenv-glx-positive-m2b.log`.
- The same llvmpipe test passed without compositor support:
  `PASS: stereo visual absent without compositor support` in
  `evidence/devenv-glx-unsupported-m2b.log`.
- Both test invocations were compiled with `-Wall -Wextra -Werror`.
- The radeonsi build completed for the modified DRI2, GLX, viewport, and
  test paths.
- The DRI3 test ran in Docker with `--device /dev/dri/renderD128
  --group-add 125`. Weston selected `/dev/dri/renderD128` and reported AMD
  radeonsi in `evidence/weston-dri3-radeonsi.log`. Source-built `glxinfo -B`
  reported direct rendering, AMD radeonsi, and Mesa 26.3.0 in
  `evidence/dri3-glxinfo-radeonsi.log`. The GLX test passed in
  `evidence/dri3-glx-positive-radeonsi.log`.
- The DRI3 run also exercises the post-switch eye resources and the doubled
  viewport at the 64-to-96 per-eye resize. Xwayland does not allow XGetImage
  on this DRI3 window, so its packed pixels were not independently read back;
  that limitation is recorded rather than treated as proof.
- A Weston headless compositor capture now provides the missing DRI3 proof.
  The run used `--device /dev/dri/renderD128 --group-add 125`, Weston selected
  that device and AMD radeonsi, and `weston-screenshooter` captured the packed
  window in `evidence/wayland-screenshot-2026-10-04_05-20-45.png`. The image
  was inspected and sampled at the red and green halves: left was
  `(255,0,0,255)` and right was `(0,255,0,255)`. The GLX test reported PASS
  in `evidence/glx-stereo-compositor-final.log`.
- For the required negative check, the two `dri_pack_stereo` source resources
  were temporarily swapped, the library was rebuilt, and the compositor
  capture in `evidence/wayland-screenshot-2026-10-04_05-19-50.png` measured
  green-left `(0,255,0,255)` and red-right `(255,0,0,255)`. The assignments
  were restored and rebuilt before the final capture. No mutation remains.
- The updated prototype geometry check is saved as
  `patches/gl-stereo-geometry.patch`; it checks the doubled X window geometry
  and no longer treats the window as a single-eye framebuffer.
- The build container has no `paraview` or `pvpython`, so a toolkit-child
  geometry test was not available. No toolkit patch was attempted.

The old external gl-stereo prototype is read-only under the house rules. Its
old full-window geometry assertion is covered by the updated in-workspace
test rather than modified in place.

Milestone 3, Mesa Vulkan WSI, is implemented in the working Mesa tree. The
WSI reports two image-array layers only when the compositor declares stereo:
X11 reads `_KDE_NET_WM_STEREO_CONTENT_SUPPORTED` from the root, and Wayland
requires both the `kde_stereo_content_manager_v1` global and `wp_viewporter`.
The application-visible image has the logical per-eye extent and two layers.
Mesa creates a double-width presentation image, packs layer 0 into the left
half and layer 1 into the right half, and declares full side by side layout 3.
On Wayland the viewport destination is the logical window size.
No program class or sub-class is declared.

Verification, all in Docker with DOCKER_HOST set to the workspace proxy:

- The standalone test compiled with `-std=c11 -Wall -Wextra -Werror`.
- The modified Mesa WSI archive built successfully with `ninja -C
  build-vulkan -j4 src/vulkan/wsi/libvulkan_wsi.a`.
- The source-built lavapipe ICD ran under Xvfb. The test checked that the
  root property absent/zero gives `maxImageArrayLayers == 1`, then checked
  that version 1 gives 2, checked the swapchain declaration property is
  CARDINAL value 3, cleared red in layer 0 and green in layer 1, and passed
  as `PASS: Vulkan stereo layers and packed present` in
  `evidence/vulkan-stereo-x11.log`.
- The same test ran through Weston Xwayland and the compositor's
  `weston-screenshooter`. The inspected capture
  `evidence/wayland-screenshot-2026-10-04_05-44-06.png` measured red
  `(255,0,0,255)` at `(790,550)` and green `(0,255,0,255)` at `(890,550)`.
- The deliberate swapped-layer run passed its own swap test and produced
  `evidence/wayland-screenshot-2026-10-04_05-44-53.png`; the same samples
  were green `(0,255,0,255)` and red `(255,0,0,255)`. The normal-eye pixel
  checker failed with status 1 on this capture, as required for a negative
  test. The run is recorded in `evidence/vulkan-stereo-compositor-swap.log`.
- The X11 and Wayland WSI sources are both included in the successful
  lavapipe target build. The native Wayland test was then run against the
  headless KWin build from branch `stereo3d`, commit `274c3da`, with KWin's
  `kde_stereo_content_manager_v1` and `wp_viewporter` globals. KWin was built
  in Docker with `cmake` and `make -j1 kwin_wayland`; its screenshot plugin
  was built with `make -j1 screenshot`.
- KWin's D-Bus `org.kde.KWin.ScreenShot2.CaptureWindow` captured the native
  Wayland window. The lavapipe run is in
  `evidence/kwin-lavapipe-window-capture.log`: capture size 128x32 for a
  64x32 logical window, with `(255,0,0)` at x=32 and `(0,255,0)` at x=96.
  The loader log identifies llvmpipe in
  `evidence/kwin-lavapipe-vulkan.log`.
- The same native Wayland test ran with the source-built RADV ICD over
  `/dev/dri/renderD128` and `--group-add 125`. The loader selected AMD Radeon
  Graphics (RADV RENOIR) from `build-common/src/amd/vulkan/libvulkan_radeon.so`.
  KWin's capture in `evidence/kwin-radv-window-capture.log` measured the same
  red-left/green-right packed result. `VULKAN_STEREO_SWAP_EYES=1` produced
  green-left/red-right in `evidence/kwin-radv-swapped-window-capture.log`,
  while the Vulkan test reported the deliberate swap PASS.
- KWin's Xwayland Vulkan path also passed the two-layer test in
  `evidence/vulkan-kwin-xwayland-capture.log`. The test accepts the valid
  `VK_SUBOPTIMAL_KHR` result returned by this headless Xwayland path.
- The common-WSI build used `-Dgallium-drivers=llvmpipe,radeonsi,iris,crocus,nouveau`
  and `-Dvulkan-drivers=amd,intel,intel_hasvk,nouveau,swrast`, then completed
  with `ninja -C build-common -j4`. The verified artifacts and SHA-256 values
  are in `evidence/common-wsi-driver-matrix.log`: llvmpipe, radeonsi, iris,
  crocus, RADV, ANV, HASVK, and NVK. Intel has no GPU on this machine, so ANV,
  HASVK, iris, and crocus have build proof only; no Intel runtime claim is
  made. NVK has build proof only as well.
- The KWin source copy contains a local, environment-gated
  `KWIN_TEST_ACTIVATE_WINDOWS=1` activation hook in `src/wayland_server.cpp`.
  It is test-harness code for deterministic capture in the no-input virtual
  backend and is not a Mesa change or an upstream KWin claim.

Milestone 3, external-driver Vulkan layer, is implemented and verified. The
layer is an implicit default-on 64-bit Vulkan layer. It hooks Wayland, XCB, and Xlib surface
creation, requires the compositor support declaration, reports two image
array layers, creates two-layer shadow images, packs them into a double-width
downstream swapchain image at queue present, and declares full side by side
layout 3. If the downstream driver already reports two layers, it leaves the
swapchain path unchanged.

Verification, all in Docker with the workspace Docker proxy:

- Mesa's Meson build accepted `-Dvulkan-layers=stereo` and built
  `src/vulkan/stereo-layer/libVkLayer_MESA_stereo.so` with `ninja -C
  build-vulkan -j4`. The generated implicit-layer manifest was used for the
  runtime proof.
- The runtime proof used the container's stock lavapipe ICD from
  `/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so`, not the modified Mesa WSI.
  The loader log confirms the layer library and stock ICD in
  `evidence/kwin-layer-final-vulkan.log`. KWin's `CaptureWindow` measured a
  128x32 packed window, red `(255,0,0)` at x=32 and green `(0,255,0)` at x=96
  in `evidence/kwin-layer-final-window-capture.log`.
- The deliberate swap used the same stock ICD and layer. KWin measured green
  `(0,255,0)` at x=32 and red `(255,0,0)` at x=96 in
  `evidence/kwin-layer-final-swapped-window-capture.log`; the test reported
  `PASS: Vulkan Wayland deliberate eye swap` in
  `evidence/kwin-layer-final-swapped-vulkan.log`.
- The layer was also loaded over the source-built Mesa lavapipe WSI. The
  WSI's own stereo path remained active and the native KWin capture passed in
  `evidence/kwin-layer-mesa-bypass3-window-capture.log`; the loader confirms
  the layer and Mesa ICD in `evidence/kwin-layer-mesa-bypass3-vulkan.log`.
- The XCB/Xlib layer hooks compile in the Meson target. The stock-lavapipe
  Xwayland test passed the X11 capability, declaration, and swap checks in
  `evidence/kwin-layer-x11-vulkan.log`. KWin's active-output capture for that
  run was a full 2048x640 output rather than a window image, so it is not
  counted as independent X11 packed-pixel proof.
- Mesa's local contribution rules were read. No GitLab action, merge request,
  push, or upstream submission was made. The real NVIDIA run remains a
  test-run item and was not claimed here.

Milestone 4, Mesa EGL multiview window surfaces, is implemented and verified.
The implementation follows EGL_EXT_multiview_window. The extension's view
count semantics fit a stereo pair for the two-view case: view 0 is the left
buffer and view 1 is the right buffer by this project's companion contract.
The registry text does not define left/right ordering or a packed presentation
format, so the companion contract declares full side by side layout 3.

Implementation:

- EGL parses EGL_MULTIVIEW_VIEW_COUNT_EXT on window surfaces, accepts two
  views, reports the requested count through eglQuerySurface and the bound
  count through eglQueryContext, and advertises the extension only when the
  compositor's stereo support is available.
- DRI2 selects the stereo DRI config for the multiview context and uses the
  existing two-eye Gallium resources. The X11 and Wayland platform paths pack
  the eyes into one double-width buffer and declare
  _KDE_NET_WM_STEREO_CONTENT value 3 or the Wayland stereo content value 3.
- The Wayland path sets a wp_viewporter destination to the logical window
  size. Its buffer copy uses the packed buffer width, so both views reach the
  compositor instead of only the left half. No class or subclass is declared.
- The same DRI2 EGL implementation builds for radeonsi, iris and crocus; no
  driver-specific EGL change was needed. Intel runtime testing is unavailable
  because this machine has no Intel GPU.

Verification, all in Docker with the workspace Docker proxy and the permitted
renderD128 device:

- The llvmpipe build completed with ninja -C build-glx -j4 for libEGL and the
  DRI swrast target. The source-built radeonsi EGL and DRI targets also
  completed with ninja -C build-radeonsi -j4. Logs are in
  evidence/egl-build-glx.log and evidence/egl-build-radeonsi.log.
- Native Wayland under the headless KWin harness ran the EGL test with a
  64x32 logical window. It reported surface 64x32, requested views 2, actual
  views 2 and GL_STEREO 1. KWin CaptureWindow measured red at x=32 and green
  at x=96 in evidence/egl-kwin-wayland-window-capture.log.
- The deliberate Wayland swap test reported swap=1 and KWin measured green at
  x=32 and red at x=96 in evidence/egl-kwin-wayland-swap-capture.log. Both
  capture and test return codes are zero in the corresponding result logs.
- The same native Wayland test passed with source-built radeonsi and reported
  the same two-view and GL_STEREO results in evidence/egl-radeonsi-wayland.log.
- The X11 test now reports the logical eye size. Under Xvfb with the
  source-built llvmpipe EGL, it reported a 64x32 EGL surface and a 128x32
  physical X11 window. It checked the default 64-pixel viewport, then set an
  explicit 128-pixel viewport and checked the packed XGetImage pixels. The
  normal run measured red `0xff0000` and green `0xff00`; the deliberate swap
  measured green then red. Both return codes are zero in
  `evidence/egl-review-x11-result.log`, `evidence/egl-review-x11.log`, and
  `evidence/egl-review-x11-swap.log`.
- A KWin full-output capture was also attempted for Xwayland. It returned the
  valid 1024x640 output but did not expose the small X11 window's pixels, so it
  is not counted as independent X11 compositor-pixel proof. The direct X11
  readback remains the X11 verification.
- NVIDIA's open-source `egl-wayland` repository was cloned into
  `egl-wayland/`. Its Wayland glue now parses two views, doubles the producer
  and dmabuf buffer width, declares content value 3, and sets a
  `wp_viewporter` destination to the logical window size. The Meson build
  completed with `compile_rc=0` in `evidence/egl-wayland-compile.log`. The
  protocol contract test completed with `contract_rc=0` in
  `evidence/egl-wayland-stereo-contract.log`; it checks the manager, content,
  viewporter and viewport signatures and has no class or subclass. The real
  NVIDIA driver run remains a test-run item.
- The external Vulkan layer's X11 path was rerun over stock lavapipe with the
  headless KWin wrapper, explicit layer loading, and the source test's
  compositor gate. It reported `PASS: Vulkan stereo layers and packed present`
  in `evidence/kwin-layer-x11-wrapper7-vulkan.log`. The KWin X11 capture
  attempts did not produce proof: `CaptureWindow` needs KWin's internal UUID,
  while active-window and active-screen captures returned cancellation. Per
  the accepted M4 review, that capture moves to the real-desktop test run and
  is not claimed as container proof here.

- The last-item notes are in `mesa/stereo-work/DXVK-VKD3D-PACKAGING.md`. They
  identify DXVK's `DxgiFactory::IsWindowedStereoEnabled`, DXVK's Vulkan
  presenter, vkd3d-proton's `vkCreateSwapchainKHR` path, and the exact user
  buffer changes needed for two layers. They also identify the Debian testing
  Mesa source baseline as `26.1.6-1`, the Mesa binary packages carrying the
  changes, the default-on `mesa-vulkan-layer-stereo` package and
  manifest, and the patched `egl-wayland` package split. This is a source and
  packaging note, not a claim that DXVK or vkd3d-proton has been modified or
  runtime-tested.

Open questions:

- DXVK/vkd3d-proton implementation and Proton runtime tests remain future
  work. The external Vulkan layer is 64-bit only by design; 32-bit Windows
  games reach it through Wine's WoW64 mode.

Second review (Claude, 2026-10-04):

- The plain GLX test no longer pre-sizes its window, sets unconditional WM
  min/max hints, or uses `GLX_STEREO_PRESIZE_FOR_WM`. It uses per-eye viewport
  sizes, waits for the compositor's asynchronous resize, and records the
  X11 geometry and declaration after map, `glXMakeCurrent`, first swap, and
  the 96-per-eye resize.
- The installed-package test used KWin `4:6.7.4-2+stereo3d12`,
  `--cap-add SYS_NICE`, and `renderD128`. It passed with `test_rc=0` and
  reported the packed stereo swap. `xwininfo` recorded 64x32 after map,
  128x32 after `glXMakeCurrent` and first swap, and 192x32 after resize.
  KWin's `getWindowInfo` recorded frame sizes 64x32, 64x32, 64x32, and
  96x32 respectively. The declaration was absent after map and CARDINAL 3
  at each later stage. Evidence is in
  `evidence/installed-kwin-glx-unmodified-test.log` and
  `evidence/installed-kwin-glx-unmodified-stages.log`.
- The cause was Mesa's order: it resized before declaring stereo, so KWin
  handled the request at scale 1. Both `dri3_glx.c` and `drisw_glx.c` now
  declare `_KDE_NET_WM_STEREO_CONTENT` before `XResizeWindow`; this is commit
  `5739e950d95`, with the report update in `02bd9e4098b`. KWin's one-eye frame
  geometry then matched its design, so no KWin patch was needed.

Packaging review (backport to Debian testing baselines) is complete for the
checked artifacts. Mesa is based on `mesa-26.1.6` and egl-wayland on `1.1.21`.

- Mesa packages are `26.1.6-1+stereo3d2`; egl-wayland packages are
  `1:1.1.21-1+stereo3d1`. The Mesa layer package is default-on for
  compositor-announced stereo and its manifest has only
  `DISABLE_LAYER_MESA_STEREO=1` under `disable_environment`.
- The layer is 64-bit only by design. Daniel dropped 32-bit builds for good
  on 2026-09-25; 32-bit Windows games reach it through Wine's WoW64 mode.
  Both source changelogs use Daniel's requested maintainer line.
- The clean second-review Mesa build used `dpkg-buildpackage -b -us -uc -d`
  and produced
  `mesa_26.1.6-1+stereo3d2_amd64.changes` and
  `mesa_26.1.6-1+stereo3d2_amd64.buildinfo`, alongside 25 Mesa binary
  packages. There are zero Mesa `+stereo3d1` debs in
  `debian-build/source/`; 25 stale Mesa debs were moved to
  `debian-build/old/`. The build record is in
  `evidence/mesa-dpkg-buildpackage-second-review-clean.log` and ends with
  `dpkg-buildpackage_clean_rc=0`.
- The testing image lacked matching `dpkg-shlibdeps` metadata for the LLVM
  development library. The build helper retried with
  `--ignore-missing-info` and added explicit `libllvm21 (>= 1:21.1.0)`;
  `dpkg-deb -f` confirms that dependency in the Gallium, Vulkan and OpenCL
  packages. The fallback diagnostics remain in the build log.
- Lintian ran on the `.changes` file and returned zero. Its warnings are
  listed in `evidence/lintian-mesa-stereo3d2-clean.log`: Debian's
  existing intentional no-soname driver/layer warnings, the existing
  screenshot-control script/manual-page warnings, and three unused baseline
  overrides. The layer package still contains its intentional lintian
  override.
- The installed-package tests used package KWin
  `4:6.7.4-2+stereo3d12`, `--cap-add SYS_NICE`, and only `renderD128`. The
  installed Wayland Vulkan test passed with red at x=512 and green at x=1536;
  its deliberate swap inverted the samples. See
  `evidence/installed-package-wayland-normal-capture.log` and
  `evidence/installed-package-wayland-swap-capture.log`.
- Installed EGL Wayland passed with two views. KWin's capture saw red-ish
  `(243,77,77)` then green-ish `(133,255,133)`, and the deliberate swap
  inverted them. See `evidence/installed-package-egl-wayland-capture.log`
  and its `-swap-` counterpart.
- Installed KWin Xwayland `glxinfo -B` reports llvmpipe and Mesa
  `26.1.6-1+stereo3d2`; `glxinfo -v` lists stereo visuals. The unmodified
  positive GLX test now passes through the installed KWin path; the negative
  GLX test and X11 EGL test also pass. The stage records are in
  `evidence/installed-kwin-glx-unmodified-stages.log` and the earlier
  visual/readback logs remain in `evidence/installed-kwin-x11-*`.
- The non-stereo Vulkan capability test passed with the default-on layer
  loaded and no `ENABLE_LAYER_MESA_STEREO` variable. It is recorded in
  `evidence/installed-kwin-x11-layer-default.log`.

No Intel or real NVIDIA runtime claim is made, no DXVK or vkd3d-proton code
was modified, and neither backport branch was pushed.

Third round (one-view sizes, version 3):

- KWin was rebuilt from Debian's 6.7.4 source with the one-view-sizes and
  synthetic ConfigureNotify changes from commits 67b31fb and 142fe64 on top
  of 1fe94e8. The binary package set is `4:6.7.4-2+stereo3d13`; the build
  returned zero and lintian on its `.changes` returned zero. Evidence is in
  `evidence/kwin-stereo3d13-build.log` and
  `evidence/lintian-kwin-stereo3d13.log`.
- Mesa's backport branch has the version-3 one-view GLX/EGL changes and the
  Vulkan X11 and layer changes in `7a391a2f735`. Test coverage and the
  version-2 fallback are committed in `5cdfb0ff28f`; the report is committed
  in `26a9e1ce897`. The source patch applies after the existing Debian stereo
  patch with zero rejects.
- The clean Debian `26.1.6-1+stereo3d3` package build returned zero in a
  freshly updated `debian:testing` container. It produced `.changes` and
  `.buildinfo`; lintian on the `.changes` returned zero. Evidence is in
  `evidence/mesa-stereo3d3-build.log` and
  `evidence/lintian-mesa-stereo3d3.log`.
- Installed GLX under KWin +13 measured 64x32 after map, 128x32 after the
  declaration, and 192x32 after the 96-pixel resize. KWin's frame was 64x32
  and then 96x32. ConfigureNotify records ended at one-view widths 64 and
  96. Evidence is in `evidence/installed-kwin-glx-third-round-test.log` and
  its stage log.
- Installed EGL X11 measured the same physical sizes, queried a 64x32 EGL
  surface with two views and declaration 3, and read red/green from the X11
  buffer. The fixed min/max run recorded `EGL_HINTS min_max=64x64`. Evidence
  is in the `evidence/installed-egl-x11-third-round*` files.
- Installed Vulkan X11 version 3 passed with one-view extents and no
  `VK_SUBOPTIMAL_KHR`. KWin capture measured 128x32 with red at x=32 and
  green at x=96; the deliberate swap inverted those samples. Evidence is in
  `evidence/installed-vulkan-x11-third-round-{normal4,swap4}*`.
- The version-2 GLX and EGL fallbacks doubled the window themselves to
  128x32 under Xvfb and returned zero. The Vulkan version-2 test retained its
  old successful path. Evidence is in the `evidence/*v2-xvfb-*` files.
- A plain X11 window selected by a KWin `sbs-full` window rule kept its X11
  geometry at 64x32 while KWin's frame was 32x32. Installed native Wayland
  Vulkan and EGL remained unchanged and both passed with KWin capture. The
  rule and Wayland evidence are in `evidence/installed-window-rule-*`,
  `evidence/installed-kwin-window-rule-*`, and the
  `evidence/installed-package-wayland-*` files.
- All third-round test sources compile with `-Wall -Wextra -Werror` in
  `evidence/third-round-test-compile-final.log`. Tests used KWin +13, Mesa
  +stereo3d3, `--cap-add SYS_NICE`, and only renderD128. No Intel or real
  NVIDIA runtime claim is made. Neither backport branch was pushed.

Fourth round: layout-independent Vulkan X11 extents:

- Undid the old layout-based extent change. The Vulkan WSI and layer no longer
  read `_KDE_NET_WM_STEREO_CONTENT` from the window to choose an extent. Each
  records a successful Mesa-owned two-layer packing swapchain and halves the
  X11 width on the next capabilities query. A program-owned one-layer packed
  swapchain keeps the complete X11 width. Source commits are
  `6695fa886d0`, `c3149cc5071`, and `160555072d8`.
- Mesa `26.1.6-1+stereo3d4` built in fresh updated `debian:testing` with
  `dpkg-buildpackage -b -us -uc -d` returning zero. Lintian on its `.changes`
  returned zero. Evidence is in
  `evidence/mesa-stereo3d4-build.log` and
  `evidence/lintian-mesa-stereo3d4.log`.
- Installed KWin +13 tests used `--cap-add SYS_NICE` and only renderD128. The
  two-layer test measured `64x32` one-view extent against a `128x32` X11
  window, and KWin capture verified red/green and the deliberate swap.
  The one-layer program-packed test measured `128x32` extent against the
  `128x32` X11 window. Evidence is in
  `evidence/installed-vulkan-x11-fourth-round-*` and
  `evidence/installed-vulkan-x11-fourth-one-layer.log`.
- The installed layer was explicitly loaded over the packaged lavapipe ICD;
  its loader record and both colour captures are in
  `evidence/installed-vulkan-x11-layer-fourth-*`.

The capture wrapper reports a signal during KWin teardown, while each Vulkan
test process returns zero and its pixel records match the expected result. No
Intel or real NVIDIA runtime claim is made, and neither backport branch was
pushed.

Fifth round: full side by side only:

- KWin's window declaration accepts only none (0) and full side by side (3).
  Half-side-by-side, top-and-bottom, checkerboard and right-eye-first window
  handling was removed while output formats remain output concerns. The output
  eye-swap state remains one per output, and the output protocol is v24.
  Source commits are `a60f604`, `16a74b0` and `0e6a0d4`, with the earlier
  output-protocol commits recorded in the KWin log. The KWin source build
  reached `[100%] Built target kwin_wayland`.
- The KWin Debian package is `4:6.7.4-2+stereo3d14`. `dpkg-buildpackage -b`
  returned zero and produced `.changes` and `.buildinfo`; lintian returned
  zero. Evidence is in `evidence/kwin-fifth-make-full.log`,
  `evidence/kwin-fifth-package-result.log`, and
  `evidence/fifth-round-kwin-lintian-rc.log`.
- The window protocol content enum is exactly `none=0` and
  `side_by_side_full=3`. The output protocol contract test covers protocol
  v24, the single output eye swap, and rejects right-first output flags. It
  ran three tests successfully in `evidence/plasma-fifth-protocol-contract.log`.
  The protocol package is `1.21.0-1+stereo3d3`, and its binary package build
  returned zero in `evidence/plasma-fifth-package-rebuild.log`.
  Lintian on its rebuilt `.changes` returned zero; the record is in
  `evidence/lintian-plasma-fifth-rc`.
- The shared declaration helper now exposes only `STEREO_NONE` and
  `STEREO_SBS_FULL`; it has no class or sub-class API and no content-type
  side channel. Its SONAME remains 1. The helper build passed with
  `-Wall -Wextra -Werror`; the Debian packages are
  `libstereo-declare1` and `libstereo-declare-dev`, version `1.1.0-1`.
  Evidence is in `evidence/stereo-declare-fifth-build-make.log`,
  `evidence/stereo-declare-package-build.log`, and
  `evidence/stereo-declare-package-lintian.log`.
- libkscreen keeps one output eye-swap property and removes right-first mode
  twins. Its package is `4:6.7.4-1+stereo3d5`; KScreen is
  `4:6.7.4-1+stereo3d6`. Both builds produced `.changes` or `.buildinfo`.
  Lintian initially found current-version symbol entries; those entries were
  recorded at upstream version `4:6.7.4`, and the rerun returned zero with
  only the existing `Original-Maintainer` warnings. Evidence is in
  `evidence/libkscreen-fifth-package-finalize-qch.log`,
  `evidence/libkscreen-fifth-symbols-rebuild.log`,
  `evidence/lintian-libkscreen-raw2.log`, and
  `evidence/kscreen-fifth-package-build.log`.
- Installed-package file checks extracted the KWin +14, protocol, helper,
  libkscreen and KScreen packages, verified the versions, KWin executable,
  protocol value 3, helper SONAME, and installed KScreen libraries. Evidence
  is in `evidence/fifth-installed-package-files.log` and
  `evidence/fifth-installed-kscreen-files.log`. A full apt-installed KWin
  headless capture was not completed: the throwaway container's apt command
  stopped after reading package lists, so no new fifth-round compositor colour
  capture is claimed here. Existing fourth-round captures remain against KWin
  +13. No push was performed.

Sixth round: review of the fifth:

- Restored the shared helper's five-argument X11 and Wayland declarations,
  including content class and sub-class metadata. The layout enum has only
  `STEREO_NONE=0` and `STEREO_SBS_FULL=3`, and invalid layouts are rejected.
  The X11 class property and Wayland content-type side channel are present.
  Commit `fa74887` is on the helper/protocol branch. The standalone and
  Debian builds passed in `evidence/stereo-declare-sixth-build.log` and
  `evidence/stereo-declare-sixth-package-build.log`; `readelf` reports SONAME
  `libstereo-declare.so.1`. Lintian returned zero, with only the two recorded
  initial-upload warnings, in `evidence/stereo-declare-sixth-lintian-final.log`.
- KWin's sixth-round branch is based on `d9dc542` and contains only this
  round's changes above it: `f12a066` and `041bd9a`. The virtual-mode oracle
  now expects 16 modes for the other case and 22 for both, after removing the
  right-first spatial twins. The KWin binary packages have version
  `4:6.7.4-2+stereo3d14`; `dpkg-deb` confirms the version and KWin binary,
  and lintian returned zero with five changelog-length warnings in
  `evidence/kwin-sixth-lintian.log`. The binary phase of the package build
  completed, but `dpkg-buildpackage` returned 2 in its final
  `dpkg-source --after-build` cleanup because the Debian patch did not reverse
  cleanly against the source snapshot; this is recorded in
  `evidence/kwin-sixth-package-build-rerun2.log`.
- The KWin unit tests were rebuilt with `BUILD_TESTING=ON` and `KWIN_BUILD_EIS=OFF`,
  then run with `QT_QPA_PLATFORM=offscreen`, surfaceless llvmpipe, and only
  `/dev/dri/renderD128` plus group 125. Virtual stereo (12), frame sequencing
  (11), all row/column/checker eye-flip shader cases (14), and mock DRM frame
  sequencing (3) passed. The complete output is in
  `evidence/kwin-sixth-unit-tests-final.log`. The mock test logged a failed
  probe of absent renderD129 but did not receive or use that device.
- Installed-package proof used disposable containers with KWin
  `4:6.7.4-2+stereo3d14`, Mesa `26.1.6-1+stereo3d4`, the helper and protocol
  packages, `--cap-add SYS_NICE`, and `/dev/dri/renderD128 --group-add 125`.
  On the first package-root run the GLX client loaded packaged Mesa while the
  Xwayland server still used the container's Mesa, and the positive test
  timed out. `LD_DEBUG=libs` records packaged `libGLX_mesa` on the client;
  overlaying the packaged Mesa into the throwaway container's `/usr` made both
  sides one Mesa and the test passed. The base image has no `glxinfo`, so a
  visual-list comparison could not be made. The corrected installed X11 run
  has `glx_positive=0 glx_negative=0 egl_x11=0 layer_default=0` in
  `evidence/installed-kwin-x11-status.log`. It records a declared window at
  128x32 with layout 3, synthetic ConfigureNotify ending at 64x32, then
  192x32 with the last notifications at 96x32. The GLX test and X11 EGL test
  both passed, and the default Vulkan layer test passed.
- The installed X11 Vulkan capture measured red then green at x=32 and x=96;
  the deliberate swap measured green then red. Both KWin captures and the
  test PASS lines are in `evidence/installed-vulkan-x11-third-round-*`.
  Installed Wayland Vulkan and EGL tests returned zero through KWin; their
  captures show red/green halves and swapped halves in
  `evidence/installed-package-wayland-*` and
  `evidence/installed-package-egl-wayland-*`. The window-rule test returned
  zero and kept its plain X11 window at 64x32 in
  `evidence/installed-window-rule-third-round.log`.
- KWin's `testStereoContent` integration test passed, including the invalid
  content value case, in `evidence/kwin-sixth-invalid-layout-test-root.log`.
  The output-mode tests passed for virtual modes and anaglyph, with no
  right-first spatial twins; the frame-sequential and shader pattern tests
  cover the output eye flip. No Intel or NVIDIA runtime claim is made. No
  push was performed.

KWin rebase and GLX fix:

- The requested stale round and build trees were removed before rebuilding.
  The remaining old `build` directory is owned by `nobody` and could not be
  removed by this user; the other named trees were removed and `/K3D` reports
  70 GB free.
- KWin branch `sixth-round` was rebased onto fork commit `fbfd8a1`. The
  rebased commits are `ef6157d` and `e6d9f23`.
- Mesa `stereo3d-26.1` contains the GLX fix in commits `6742105b7b0`,
  `62a972b0d7f` and `cc3b9a32cda`: mono double-buffered configs remain beside
  stereo copies, GLX declares and packs only after right-eye use, and drawable
  sizes remain in the program's one-view units. The source build completed with
  llvmpipe and iris in `evidence/glx-fix-source-build.log`.
- The Debian backport source is Mesa `26.1.6-1+stereo3d5`. Its changelog and
  `debian/patches/0003-glx-mono-and-lazy-stereo.patch` carry the three GLX
  commits; patch application was validated against the Mesa branch. The final
  Ninja build returned zero in `evidence/mesa-stereo3d5-ninja-final.rc`, and
  the binary package phase under fakeroot returned zero in
  `evidence/mesa-stereo3d5-rules-binary-fakeroot.rc`.
- The package set contains 25 amd64 packages, including `libglx-mesa0` and
  `mesa-vulkan-layer-stereo`, all at `26.1.6-1+stereo3d5`. The generated
  `.dsc`, `.debian.tar.xz`, `.changes` and `.buildinfo` are in
  `debian-build/source/`. `dpkg-deb` confirms the GLX library and the layer
  manifest's only environment control is `DISABLE_LAYER_MESA_STEREO=1`.
  Lintian on the `.changes` returned zero; its existing Mesa override and
  shared-library warnings are recorded in `evidence/mesa-stereo3d5-lintian.log`.
- Installed-package GLX proof used all `+stereo3d5` packages in the disposable
  Debian testing build container with Xvfb and llvmpipe. `glxinfo -B` reports
  Mesa `26.1.6-1+stereo3d5` and llvmpipe. The test status is
  `mono=0 mono_negative=0 stereo=0 stereo_negative=0` in
  `evidence/mesa-stereo3d5-installed-glx-status.log`; the stereo log records
  64x32 before the first swap, 128x32 after packing and 192x32 after the
  96-per-eye resize, ending with its PASS line. `glxgears` ran for three
  seconds and returned the intentional timeout code 124. The detailed logs are
  `evidence/mesa-stereo3d5-installed-glx-mono-final.log`,
  `evidence/mesa-stereo3d5-installed-glx-stereo-final.log` and
  `evidence/mesa-stereo3d5-installed-glxgears-final.log`.
- The package build used the preserved configured tree and the Debian binary
  phase, then generated the binary build records after cleaning the staging
  tree. A fresh from-scratch `dpkg-buildpackage -b` was not rerun after the
  long full compile; this distinction is recorded rather than claimed away.

Item 3: three-source investigation (2026-10-05):

Point 1, the 32-bit child attributes:

- The test now finds an ARGB visual, creates a colormap for that visual, and
  creates the child with `CWColormap | CWBorderPixel | CWBackPixel`. The
  relevant code is `tests/x11-child-stereo.c:191-208` and the tracked copy is
  `mesa/stereo-work/tests/x11-child-stereo.c:191-208`. This is the required
  X11 fix for a child whose visual/depth differs from its 24-bit parent.
- The client reached `child_property ... format=32 count=1`, drew both eye
  buffers, resized, and destroyed the windows without `BadMatch` in the
  AddressSanitizer run. The run is in `evidence/item3-asan3-client.log`.
  The same test also had an unrelated null `child_return` passed to
  `XTranslateCoordinates`; both copies now pass a real output at line 135.
  The normal KWin run still terminated before the resize, so it is not a
  passing child proof.

Point 2, the manual redirect owner:

- KWin's existing compositor request is one root-subwindow manual redirect
  on its X11 connection, at `kwin/src/xwayland/xwayland.cpp:468-470`.
  The child change sends one `xcb_composite_redirect_window` request when a
  child first enters the declaration set, at
  `kwin/src/x11window.cpp:2069-2077`; `m_declaredStereoChildren` prevents a
  second request. The old code sent the request on every update, which
  explains the repeated `BadAccess` report, but this run did not independently
  prove which server owner produced the first error.
- The GLX child test and Mesa contain no Composite redirect call. The small
  stock-client probe is intentionally separate in
  `tests/composite-child-redirect.c:1-35`; it redirects only its own child.
  It was prepared but not run: `kwin-declare-build:1` has no
  `libXcomposite` or Xvfb, and installing those packages in a disposable
  container did not complete. Therefore the general stock-session result is
  unverified.

Point 3, Xwayland surface creation and the serial path:

- In Xwayland 24.1.13, `hw/xwayland/xwayland-window.c:1459-1557` contains
  `ensure_surface_for_window()`. In rootless mode, lines `1471-1475` return
  unless `window->redirectDraw == RedirectDrawManual`. The realize path calls
  it at line `1607`; the window-pixmap hook calls it at line `1798`.
  The source selection for a non-top-level surface starts at `toplevel` at
  line `1378`, walks descendants at `1387-1413`, excludes depth-32 children
  at `1403-1408`, stops at a manually redirected descendant at `1410-1411`,
  and stores the selection at `1448-1449`.
- The announcement is not keyed by child XID. `send_window_client_message`
  uses the toplevel drawable at `732-750`; `send_surface_id_event_serial`
  allocates the serial, sends `WL_SURFACE_SERIAL`, and sets the protocol
  serial at `754-772`. The line-by-line source record is
  `evidence/xwayland-24.1.13-lines.txt`.
- KWin now treats the Xwayland serial as a serial: top-level association is
  `kwin/src/wayland_server.cpp:368-387`, while a `WL_SURFACE_SERIAL`
  ClientMessage is matched by its event-window child XID and serial lookup in
  `kwin/src/events.cpp:143-152` and `251-263`. The child redirect/remap is at
  `kwin/src/x11window.cpp:2069-2077`.
- The rebuilt KWin run still logged no child serial or child surface. It
  logged `redirect and remap stereo child` and then a null child surface;
  KWin's captures stayed blue at all samples. The records are
  `evidence/item3-kwin.log`, `evidence/item3-client.log`, and the three
  `evidence/item3-capture-*.log` files. The child was already realized before
  KWin's declaration-driven redirect, and the observed remap did not produce
  either the realize call at line 1607 or a pixmap-hook call at line 1798.
  Thus the exact remaining Xwayland condition is the rootless gate at
  lines 1471-1475: no child surface is created unless the child is manually
  redirected when one of those two hooks runs. There is no child-surface proof
  yet, and I am not replacing it with a squeezed child view.

KWin was rebuilt successfully in Docker with the local protocol fork and only
`/dev/dri/renderD128 --group-add 125`; no host display was used. The Mesa test
change is committed as Mesa `a0bc2d73988`, and the KWin association/redirect
changes are committed as `8be4609e`. The experiment below records the
requested source-level result and leaves the next design decision with review.

Item 3: Xwayland child-surface experiment (2026-10-05):

- The 24.1.13 source is on branch `stereo3d` in `xwayland`, commit
  `a43a7fd`. The one-condition change is in
  `hw/xwayland/xwayland-window.c:1467-1470`: an ancestor surface is reused
  unless the current window is manually redirected and the found surface
  belongs to that same window. The latter case falls through to the existing
  rootless redirect check and creates a new surface. `git diff --check` and
  the Xwayland build passed.
- The test image was built from Debian testing's Xwayland build dependencies
  with `apt-get build-dep xwayland`; the source is the upstream 24.1.13 tag
  (`Xwayland -version` reports `24.1.13 (12401013)`). The build used Meson,
  Ninja and four CPUs, with `xkb_dir=/usr/share/X11/xkb` and
  `xkb_bin_dir=/usr/bin`. The resulting binary is
  `xwayland-build2/hw/xwayland/Xwayland`. The runtime container used that
  binary through `PATH`, the host `xkbcomp` binary mounted read-only, the
  existing KWin build and the existing Mesa build.
- The child test gained an explicit `--predeclare` mode in Mesa commit
  `03645139cb3`; its normal mode still declares through Mesa. Both modes ran
  through the custom Xwayland binary under the headless KWin harness. The
  client completed creation, both eye draws, input probe, resize, second eye
  draw and destruction. The predeclare run is in
  `evidence/item3-xwl-client.log`; its Xwayland/KWin run is in
  `evidence/item3-xwl-kwin.log`.
- The compositor captures did not contain `red_at` or `green_at`. The
  requested child colours were absent at the child coordinates in
  `evidence/item3-xwl-capture-first.log` and
  `evidence/item3-xwl-capture-resize.log`; after destruction the same region
  was blue in `evidence/item3-xwl-capture-destroyed.log`. Therefore the
  experiment did not produce the requested capture proof, including with the
  32-bit controls child above it.
- The remaining design conflict is the call order. The ancestor is found at
  `xwayland-window.c:1467-1469` during child realization, before KWin's later
  declaration-driven manual redirect. The child then returns the ancestor
  surface. The only existing later entry points are
  `xwl_realize_window():1607` and `xwl_window_set_window_pixmap():1798`.
  KWin's unmap/map after redirect did not invoke either entry point: no child
  `WL_SURFACE_SERIAL` reached KWin and no child surface was associated. The
  one-condition change is therefore built and tested, but Xwayland's
  one-surface-per-tree realization order still fights the current declaration
  timing. No squeezed two-view child was used.

Item 3: instrumented ordering and association fix (2026-10-05):

- The previous experiment paragraph's conclusion about a missing hook was
  superseded by the instrumented run. The corrected Xwayland condition is
  `xwl_window->toplevel == window`, not a `surface_window` comparison. It is
  committed as Xwayland `654e8f4`. The clean Xwayland build returned zero.
- `composite/compalloc.c` shows the relevant order. The single-manual-client
  BadAccess check is in `compRedirectWindow()` at lines 152-158; it calls
  `compCheckRedirect()` at lines 220-223. `compAllocPixmap()` sets
  `pWin->redirectDraw` at lines 615-618 and calls `compSetPixmap()` at
  620. `compSetPixmap()` reaches the screen's `SetWindowPixmap` hook through
  `compwindow.c:144-154`. Thus KWin's redirect is already manual when the
  Xwayland pixmap hook runs in this test; no second remap was needed to make
  the hook see the child.
- The instrumented run proves KWin's request succeeded:
  `evidence/item3-xwl-kwin.log:108` records child `4194309` with error `0`.
  Xwayland then sees the child with `redirect=2`, continues past the ancestor,
  creates a surface with `toplevel=0x400005`, and returns that surface at
  `evidence/item3-xwl-kwin.log:114-122`. The child hook therefore works.
- The actual failing step was serial timing. KWin received child serials 2 and
  3 before the Wayland shell had applied them:
  `evidence/item3-xwl-kwin.log:124-137` records `surface missing` and then
  `wayland serial applied`. The child-specific handler consumed the message,
  so the later `surfaceAssociated` signal had no tracked child to attach. The
  client itself completed both frames, resize and destruction in
  `evidence/item3-xwl-client.log:1-9`.
- KWin now retains a missing child serial and resolves it when the shell emits
  `surfaceAssociated`, in `src/events.cpp:251-266`,
  `src/wayland_server.cpp:368-394`, and
  `src/x11window.cpp:2043-2149`. The clean KWin build returned zero. The
  instrumented rerun reached `surface associated child 3` at line 139,
  confirming the repaired association path. This is committed as KWin
  `37be59d`.
- The same run did not yet provide the requested colour capture. After the
  association, KWin reported the child surface as unmapped with size 0 at
  lines 140 and 162-174, and the capture logs contain no `red_at` or
  `green_at`. The exact repaired step is therefore verified, but the later
  child-buffer/DRI3 presentation step remains unverified and is not claimed
  as passing. No squeezed child view was used.

Item 3: child damage registration and posting (2026-10-05):

- Xwayland now registers damage for a manually redirected child that owns the
  new surface in `xwayland/hw/xwayland/xwayland-window.c:1613-1618`. It seeds
  the existing window region with `DamageDamageRegion`, so a surface created
  after the child was already drawn enters the same `damage_report()` path as
  a top-level. `damage_report()` queues the surface at lines `296-325`;
  `xwl_window_post_damage()` attaches the pixmap and commits it at
  `2082-2092`. The source-window walk keeps the child's own pixmap and
  registers its damage at `1376-1456`.
- The change is committed in the Xwayland `stereo3d` branch as `488d32d`
  (`xwayland: post damage for redirected child surfaces`). The rebuilt
  `xwayland-build2/hw/xwayland/Xwayland` completed successfully. Temporary
  Xwayland, KWin and Present logging was removed before the commit.
- The instrumented run showed the child path posting and committing buffers:
  `evidence/item3-xwl-kwin.log:48-50` reports the initial child
  `128x96` pixmap and `58-60` reports the resized `160x80` pixmap. KWin's
  parent capture returned to blue at the child area after destruction in
  `evidence/item3-xwl-capture-destroyed.log:15-23`, confirming the redirected
  child is not stale paint left in the parent's picture. The child client
  completed both frames, resize and destruction in
  `evidence/item3-xwl-client.log:1-9`.
- Present was checked with temporary lines in `xwl_present_check_flip()`,
  `xwl_present_flip()` and the `present_execute_copy()` path. No Present line
  appeared in the run: this GLX path reaches Xwayland through damage posting,
  not the Xwayland Present callbacks. Composite's source confirms that a
  manually redirected window gets `redirectDraw` before `compSetPixmap()`:
  `xwayland/composite/compalloc.c:152-158` checks the single manual owner and
  `615-626` sets the redirect and installs the pixmap. The automatic parent
  copy is `PictOpSrc` at `xwayland/composite/compwindow.c:672-723`.
- The requested red-left/green-right capture is still not proven. The exact
  next failing step is visible before Xwayland: the child pixmaps handed to
  it are only one-view wide (`128x96` and `160x80`), not packed widths
  (`256x96` and `320x80`). This is recorded in the temporary attach lines
  above. The resulting captures contain no `red_at` or `green_at` and the
  child-area samples are blue again after removal; no 3D pixels are claimed.
  The remaining fix belongs to Mesa's DRI3 stereo allocation/presentation
  path, not to the child's Xwayland surface or damage registration. The
  associated temporary KWin source instrumentation is removed and KWin's
  working tree is clean.

URGENT Mesa GLX fix and +stereo3d6 (2026-10-05):

- Mesa commits `5d07d5c41c2` and `58eb9cb8013` contain the DRI3 activation and
  mono-selection fixes. `git -C mesa status --short` is clean.
- The valid build-tree GPU trace is `evidence/glx-dri3-trace-build-gpu2.log`.
  It reports radeonsi, OpenGL `Mesa 26.1.6`, loaded
  `/usr/lib/x86_64-linux-gnu/libGLX_mesa.so.0.0.0` and
  `/usr/lib/x86_64-linux-gnu/libgallium-26.1.6.so`. It records a stereo
  config, right-eye renderbuffer activation, support version 3, property 3,
  and final window geometry `600 300`.
- That trace identifies no Mesa code failure on DRI3: the chosen config is
  stereo, the drawable flag is set, `st_manager_add_color_renderbuffer()`
  activates stereo, `glx_activate_stereo()` runs, and the resize/property
  reaches Xwayland. The earlier installed GPU failure was caused by the
  package build using source patches `0004` and `0005` only in `series`; they
  were not applied in the package build tree (`quilt applied` stopped at
  `0003`). The package source was then manually brought to the same source
  state, but that is not a clean Debian rebuild.
- A fresh `dpkg-buildpackage` was attempted in the Debian build container.
  It first failed at the stale Meson option `dri-drivers-path`, then at the
  existing build graph's missing `libclang-cpp.so`, `libDirectX-Guids.a`,
  `libd3dx12-format-properties.a`, `valgrind.h`, and Rust/generated inputs.
  The failures are recorded in `debian-build/source/mesa-26.1.6/pkg-run.log`,
  `pkg-run2.log`, `pkg-run3.log`, and `pkg-run4.log`. No clean +6
  `.changes`/`.buildinfo` pair was produced after applying 0004/0005.
- A temporary binary repack was made only to separate test results from the
  stale +6 packages. The installed software run passed in
  `evidence/mesa6-repacked-latest-software.log`: mono `300 300`, undeclared;
  stereo `600 300`, declaration `3`. It is not a Debian source rebuild.
- The corresponding installed GPU run used exactly
  `--device /dev/dri/renderD128 --group-add 125`, and the device was visible
  to user `t` as group 125. Its KWin log is
  `evidence/mesa6-kwin-gpu-final6.log` and its client result is
  `evidence/mesa6-gpu-final6.log`. KWin reached the GPU backend, but this
  temporary repack's GLX client had no RGB visual and therefore produced no
  smoke result. The repack changed SONAME/version metadata while assembling
  binaries from the build-tree stage; it is not acceptable package proof.
- The required package gate is therefore blocked on a clean Debian backport
  build with 0004/0005 applied, followed by the installed GPU smoke and Qt
  mono test. The build-tree proof is complete; the package proof is not.

Clean +stereo3d6 package rebuild and gate (2026-10-05)

The preceding urgent section records the stale-package failure. It is
superseded by this clean rebuild; the old tree was not patched further and no
binary repack was used.

- Before rebuilding, the old `debian-build/source/mesa-26.1.6`,
  `debian-build/repacked` and `debian-build/repack-layer` trees were removed.
  `/K3D` then had 82 GB free; it has 73 GB free after the build, above the
  40 GB floor. The source output now contains only the new `+stereo3d6` set.
- `Dockerfile.mesa-stereo-debbuild` builds `mesa-stereo-debbuild:1` from
  `debian:testing`, enables deb-src, installs Debian's Mesa build-depends,
  `devscripts`, `quilt`, `fakeroot`, `lintian` and the build tools. The image
  build log is `evidence/mesa-stereo-debbuild-image.log`. `dpkg-checkbuilddeps`
  completed in the image for the extracted source.
- A new source tree was extracted from
  `debian-build/source/mesa_26.1.6-1.dsc` at
  `debian-build/mesa-26.1.6-clean`. No previous build tree was reused. The
  five project patches were added to Debian's series after Debian's two
  existing patches. `evidence/mesa6-clean-quilt-applied.log` records
  `0001-stereo3d.patch`, `0002-one-view-sizes-vulkan-x11.patch`,
  `0003-glx-mono-and-lazy-stereo.patch`, `0004-glx-dri3-stereo.patch` and
  `0005-glx-mono-selection.patch` applied, in that order.
- `dpkg-buildpackage -b -uc -us` completed in the fresh source tree with
  `mesa-stereo-debbuild:1`, using at most four CPUs. The full log is
  `evidence/mesa6-final-build.log`; its tail contains the generated
  `mesa_26.1.6-1+stereo3d6_amd64.buildinfo` and `.changes`. It produced 25
  amd64 packages, including the separate
  `mesa-vulkan-layer-stereo` package. The layer is not duplicated in
  `mesa-vulkan-drivers`.
- The layer package metadata in the copied source set is version
  `26.1.6-1+stereo3d6` and depends on the matching Mesa drivers. Its manifest
  has only `disable_environment` with `DISABLE_LAYER_MESA_STEREO=1`; it has
  no enable switch. `evidence/mesa6-final-glx-symbols.log` shows the DRI3
  stereo symbols and the final libgallium versioned dependency in
  `libGLX_mesa.so`. `dpkg-deb -I` on the final package reports
  `26.1.6-1+stereo3d6`.
- Lintian ran on the binary `.changes` with exit status 0;
  `evidence/mesa6-final-lintian.log` contains only warnings and the exit code
  is in `evidence/mesa6-final-lintian.rc`.
- The exact requested smoke commands both returned 0:
  `evidence/mesa6-final-exact-smoke-software.rc` and
  `evidence/mesa6-final-exact-smoke-gpu.rc`. The detailed installed-package
  software run is `evidence/mesa6-final-package-software.log`: the mono
  window is `300x300` and undeclared, while the stereo window is `600x300`
  and declares layout `3`.
- The corrected GPU run passed only `--device /dev/dri/renderD128` and
  `--group-add 125`. `evidence/mesa6-gpu-geometry.log` records radeonsi,
  the stereo root announcement, mono `300x300` undeclared, and stereo
  `600x300` declared `3`; its return code is 0. The renderer/version and
  loaded-library proof is in `evidence/mesa6-gpu-proof-glxinfo.log` and
  `evidence/mesa6-gpu-proof-maps.log`: radeonsi, OpenGL `Mesa
  26.1.6-1+stereo3d6`, `/usr/lib/x86_64-linux-gnu/libGLX_mesa.so.0.0.0` and
  `/usr/lib/x86_64-linux-gnu/libgallium-26.1.6-1+stereo3d6.so`.
- The GPU Qt mono check returned 0. `evidence/mesa6-qt-gpu-result.log`
  records a `300x300` window with no stereo declaration, and
  `evidence/mesa6-qt-gpu-client.log` records DRI3, radeonsi and
  `requested_stereo=0 context_stereo=0`.
- The new packages replaced the stale `+stereo3d6` files in
  `debian-build/source`. There are 25 matching packages there, and the
  copied `libglx-mesa0` has the same SHA-256 as the build output:
  `0db88989749b0d59db8af71085029b7a4ca6091b07c302f71f8bbe9fe54f8f23`.

The preceding package section is the accepted `+stereo3d6` hand-back. Its
next-step note is superseded below by the clean `+stereo3d7` child test.

Item 3: redirected X11 child at two views' width (2026-10-05)

- The KWin branch was already rebased onto the fork's `stereo3d` commit
  `fbfd8a1`. `git merge-base --is-ancestor fbfd8a1 kwin` succeeds. The child
  implementation is in `fd8144f`; it redirects all children while a stereo
  child is declared, keeps the stereo child below an undeclared child, and
  updates the child surface with its one-view destination size.
- The Xwayland branch contains `654e8f4` (a manually redirected child gets
  its own surface), `488d32d` (the same damage registration and posting path
  as a top-level), and `3b39ffc` (32-bit root-surface alpha preservation).
  The damage registration is at `hw/xwayland/xwayland-window.c:1615-1620`.
  The pixmap hook calls `ensure_surface_for_window()` and disposes changed
  buffers at `:1787-1813`. The test binary uses this clean source build;
  no temporary Xwayland or KWin logging remains.
- Mesa `+stereo3d6` passed the accepted GLX smoke gate, but it did not contain
  the later child-buffer commit. A clean fresh Debian source build with that
  commit was therefore made as `26.1.6-1+stereo3d7`; no old build tree or
  binary repack was used. The source patch list is recorded in
  `evidence/mesa7-quilt-final.log`, the build records are the `mesa7` files,
  `evidence/mesa7-lintian.rc` is `0`, and `dpkg-deb -I` reports
  `libglx-mesa0` version `26.1.6-1+stereo3d7`. The installed map proof names
  both `libGLX_mesa.so.0.0.0` and
  `libgallium-26.1.6-1+stereo3d7.so` in
  `evidence/item3-package-gpu-final-mesa-maps.log`.
- The test change is committed in Mesa as `f25c7ab27ed` (`tests: cover
  redirected stereo child layers`). It waits for the predeclared child to
  receive KWin's doubled X11 width before the first draw, recording the real
  and synthetic ConfigureNotify sizes. It renders the alpha controls child
  through an ARGB XRender pixmap, because core `XPutImage` made the source
  opaque instead of preserving alpha.
- The installed-package software run returned `0` in
  `evidence/item3-package-software-final.rc`. Its client reports
  `Mesa 26.1.6-1+stereo3d7` and llvmpipe, with red in back-left and green in
  back-right. The first capture has blue outside the child, red in the left
  child half and green in the right child half, with
  `color_samples red=2544 green=2544 blue=321536`; the controls overlay is
  `(255,128,128)` over red and `(128,255,128)` over green. The resize capture
  shows the deliberate blue/cyan swap, and the destroyed capture is blue in
  both eyes. Evidence is in `evidence/item3-package-software-final-*`.
- The installed-package GPU run used only `--device /dev/dri/renderD128`
  and `--group-add 125`, returned `0` in
  `evidence/item3-package-gpu-final.rc`, and reports radeonsi plus
  `Mesa 26.1.6-1+stereo3d7` in
  `evidence/item3-package-gpu-final-client.log`. Its first KWin capture has
  the same red/green child result and alpha samples as software. Its resize
  and destroyed captures show the swapped blue/cyan frame and then the
  blue parent in both eyes. Evidence is in
  `evidence/item3-package-gpu-final-*`.
- An untimed first GPU draw can race KWin's doubled child ConfigureNotify;
  it presents a one-view pixmap. The test now waits for the declared
  pre-mapped child to reach `256x96` before drawing. The timed run then
  passed on both paths, and the client logs show the expected real
  `256x96` followed by synthetic `128x96`, and after resize `320x80`
  followed by synthetic `160x80`.
- The parent has no child hole after destruction, and the first capture has
  the same blue parent outside the child in both eyes. Input probing was
  skipped in this headless run (`SKIP_INPUT_PROBE=1`), so pointer delivery
  was not verified here. The next queue item is packaging the clean
  Xwayland branch as `+stereo3d1`, then the flush, viewport and right-buffer
  checks.

Xwayland package +stereo3d1 (2026-10-05)

- The Xwayland branch starts at xwayland-24.1.13 commit
  `c5a47fd4e3810a15632789b3291b1fc00db50f0f`. Four commits were exported
  with `git format-patch`: redirected children get their own surfaces,
  manually redirected children get their own surfaces, child damage is
  posted through the top-level path, and 32-bit root surfaces retain alpha.
  The export and its base are documented in
  `evidence/xwayland-upstream-README.md`. The external repository contains
  only this requested folder in commit `6591eec`.
- The Debian source was freshly extracted at `2:24.1.13-1`. Quilt applied all
  four patches in order, and the changelog version is
  `2:24.1.13-1+stereo3d1`; the record is
  `evidence/xwayland-quilt.log`. `dpkg-buildpackage -b -uc -us` completed in
  the fresh `mesa-stereo-xwayland-debbuild:1` image. The binary package,
  `.changes` and `.buildinfo` are in the workspace, and the binary metadata
  reports version `2:24.1.13-1+stereo3d1`.
- `dpkg-buildpackage -S -d -uc -us` also produced the source `.dsc`, Debian
  tarball and original tarball. A second empty-tree `dpkg-source -x` proof
  applied all four patches; `evidence/xwayland-source-proof2.log` records
  it. The three source files were copied to
  `/K3D/temp/sparky-os/repo/source/`. The binary `.changes` passed lintian
  with exit status 0. The stale `xwayland_24.1.13-1.*` files from the apt
  source download remain only in the workspace and are not the published
  +stereo3d1 set.
- The installed +stereo3d1 binary was exercised with the child test under a
  freshly built KWin `fd8144f` test binary, once with llvmpipe and once with
  radeonsi using only renderD128 and group 125. Both returned 0. The client
  logs report Mesa `26.1.6-1+stereo3d7`, red left and green right in the
  child's XImage, the real doubled child geometry (`256x96`), and KWin's
  synthetic one-view ConfigureNotify (`128x96`); the GPU record is
  `evidence/item3-gpu-client.log` and the software record is
  `evidence/item3-software-client.log`. KWin's screenshot helper was
  cancelled in this direct build because the test image lacks its screenshot
  service, so these runs are not compositor-capture proof. The earlier run
  with installed KWin +13+1 is not used because that package predates
  `fd8144f`.
- The temporary KWin build was made with `BUILD_TESTING=OFF` from the clean
  `kwin` branch at `fd8144f`, using Debian testing build dependencies and the
  workspace protocol XML. Its build log is
  `evidence/kwin-xwayland-build.log`. It was a test binary only, not a
  package or repository change.

i386 release-blocker discovery (2026-10-05)

- The clean simulation pins both repository names, `Sparky Stereo OS` and
  `Sparky Stereo`, while also pinning SparkyLinux. The local repository still
  exposes the accepted Mesa +stereo3d6 packages, not +stereo3d7. With the
  current amd64-only set, `apt-get -s --install-recommends install
  steam-installer` returned 100; its solver explicitly selected
  `libgl1-mesa-dri:amd64=26.1.6-1+stereo3d6` and rejected the missing i386
  twin. This reproduces the release blocker in
  `evidence/i386-sim2-steam-installer.log`.
- `apt-get -s --install-recommends install lutris` returned 0 in the clean
  simulation. The Wine simulation could not locate `wine32:i386` in the
  current Sparky index even though the repository study records Wine 10.0;
  that is an external repository/index state to resolve before claiming the
  required Wine proof. The relevant results are
  `evidence/i386-sim2-lutris.log` and
  `evidence/i386-sim-wine-final.log`.
- Mesa +stereo3d7 was rebuilt natively with `--platform linux/386` from a
  freshly extracted source tree. `dpkg-buildpackage -B -a i386 -uc -us -j4`
  produced 23 i386 packages, the `.changes` and `.buildinfo`; the full log is
  `evidence/mesa7-i386-build.log`. The source `.dsc`, Debian tarball and
  original tarball are in `/K3D/temp/sparky-os/repo/source/`.
- FFmpeg `7:9.0.2-1+stereo3d2` was rebuilt natively with the same command and
  `DEB_BUILD_OPTIONS=nocheck`. It produced 34 i386 binary packages plus
  `.changes` and `.buildinfo`; `evidence/ffmpeg-i386-build.log` ends with the
  binary-only upload. Its source package is in the repository source folder.
- A local apt overlay containing Mesa +stereo3d7 for both architectures was
  used for the solver check. Direct local-package simulations of
  `steam-installer` and `lutris` both returned 0 and selected the matching
  Mesa version for amd64 and i386; the full record is
  `evidence/i386-sim-direct.log` and its summary is
  `evidence/i386-sim-direct-summary.log`. The ordinary local-index simulation
  was not used as proof because apt's file-index refresh raced the Docker
  proxy and retained Debian's Mesa candidate.
- The live SparkyLinux `tiamat/main/binary-i386/Packages.gz` still has no
  `wine32` stanza. It has `wine` 10.0~repack-12 and `wine64` only in the
  amd64 index. Therefore `wine32:i386` cannot yet be installed from Sparky's
  current i386 index, and the Wine simulation and real Wine test remain open.
- The real `steam-installer` installation, 32-bit OpenGL/Vulkan runs on both
  GPU and software paths, and the final amd64 stereo smoke test have not yet
  been run. The i386 milestone is therefore not complete.

i386 release-blocker work (2026-10-06)

- Mesa +stereo3d7 was rebuilt natively on `--platform linux/386` with
  `dpkg-buildpackage -B -a i386 -uc -us -j4`. It produced 23 i386 packages,
  `.changes` and `.buildinfo`; the packages are in
  `debian-build/mesa7-i386-debs/`. The source package is in
  `/K3D/temp/sparky-os/repo/source/`.
- FFmpeg `7:9.0.2-1+stereo3d2` was rebuilt natively with the same command and
  `DEB_BUILD_OPTIONS=nocheck`. It produced 34 i386 packages plus `.changes`
  and `.buildinfo` in `debian-build/ffmpeg-i386-debs/`. The build log is
  `evidence/ffmpeg-i386-build.log`, and its source package is in the
  repository source folder.
- The direct-package solver proof selected matching Mesa +stereo3d7 packages
  for both architectures. The `steam-installer` and `lutris` simulations
  returned 0; the recorded plan is `evidence/i386-sim-direct.log` and the
  return-code summary is `evidence/i386-sim-direct-summary.log`. The live
  SparkyLinux i386 index was checked directly and still has no `wine32`
  stanza, so the requested Wine simulation cannot be made valid from the
  current Sparky index.
- A real native-i386 container was run with `--device /dev/dri/renderD128`
  and `--group-add 125`. `steam-installer` installed with return code 0 and
  was never started. The installed Mesa versions for both amd64 and i386,
  and the Steam version, are in `evidence/i386-installed-package-versions.log`;
  the full install log is `evidence/i386-steam-install.log`.
- The 32-bit `vulkaninfo` executable is an ELF i386 binary. With the device
  attached, Vulkan returned 0 and identified RADV on AMD Radeon Graphics with
  Mesa `26.1.6-1+stereo3d7`; forcing the i386 lavapipe ICD returned 0 and
  identified llvmpipe with the same Mesa version. The complete GPU/software
  output is `evidence/i386-gpu-software-tests.log`.
- The 32-bit `glxinfo` executable ran under Xvfb and returned 0 on both
  command paths, but Xvfb reports no DRI3 and llvmpipe for both. This is not
  GPU OpenGL proof. A direct 32-bit EGL probe opened the radeonsi driver on
  renderD128 but failed `eglInitialize` with `EGL_NOT_INITIALIZED` because
  the surfaceless/GBM probe had no usable EGL config; that output is in
  `evidence/i386-egl-tests.log` and `evidence/i386-gbm-gl-test.log`.
- The milestone is blocked by two unresolved gates: Sparky's current i386
  index does not provide the mandated `wine32:i386`, and a compositor-backed
  32-bit GPU OpenGL run has not been established. The disposable install
  container was stopped. `/K3D` has 61 GB free. Old superseded workspace
  trees removed earlier were `glx-fix-build`, `glx-fix-install`,
  `kwin-item3-build-user`, and `debian-build/mesa-26.1.6-clean`; the
  remaining `build` and `kwin-item3-build` directories were not removable
  under the current ownership and were left untouched.

Step 3: pending-package publication (2026-10-06)

- The accepted non-debug i386 Mesa +stereo3d7 binaries (23 packages) and
  FFmpeg +stereo3d2 binaries (18 packages) were copied into
  `/K3D/temp/sparky-os/pending-packages/`. Their source packages are present
  in `/K3D/temp/sparky-os/repo/source/` as the Mesa, FFmpeg and original
  source records.
- Xwayland `2:24.1.13-1+stereo3d1` exists. Its non-debug amd64 binary was
  copied to `pending-packages/`; its `.dsc`, Debian tarball and original
  tarball are already in `repo/source/`. The four-patch export remains in
  the committed external folder at `6591eec`.
- The exact published filenames are recorded in
  `evidence/step3-published-files.log`. Debug-symbol packages were left out,
  matching the existing pending-package convention.
- Step 3 is complete. No repository index was changed in this step; the next
  queue item is the flush, viewport, right-buffer, KiCad/GRASS and pointer
  re-checks.

## KWin synthetic ConfigureNotify item (2026-10-07, completed)

- The corrected Krita proof used the repository packages `krita`, `krita-data`
  and `krita-l10n` at `1:6.0.4+dfsg-1+stereo3d5`, with the splash enabled,
  patched KWin `ad122faa`, Xwayland and extracted Mesa +stereo3d8 libraries
  in a disposable headless container. The X11 watcher recorded Krita at
  one-eye `852x739`, the stereo declaration, a real doubled `1704x739`
  ConfigureNotify, and the synthetic one-eye `852x739` event. A later real
  doubled configure delivered with the root configure was followed by
  another synthetic `852x739` event. Final `xwininfo` reports the expected
  X11 render window `1704x739`; the client-facing sequence proves the UI was
  not left at the doubled width. Evidence is in
  `evidence/krita-configure-watch.xcb.log`,
  `evidence/krita-configure-watch.windows.txt`,
  `evidence/krita-configure-watch.client.log` and
  `evidence/krita-configure-installed3-apt.log`.
- The local X11 integration test was rerun after the final root-event
  condition change. `testRandrEmulation(normal)` and `(scaled2x)` passed;
  totals were 4 passed, 0 failed. Evidence is in
  `evidence/kwin-configure-test-root-capture4.log` and `.rc`.
- The KWin branch remains clean at committed `ad122faa14a16dbb446bc4bd3d9aca9f238a8f8`.
  It is based on `stereo3d` `5ce2eed63e945de07aa9043ec765a8506af26c10`.
- KWin `4:6.7.4-2+stereo3d15` was built in a fresh updated
  `debian:testing` image with Qt `6.11.2` and KDE Frameworks `6.30.0` build
  dependencies. The binary build returned 0 in
  `evidence/kwin-stereo3d15-build.log`; the outer disposable-container
  timeout fired during teardown after that successful build result. Lintian
  on the `.changes` returned 0 in
  `evidence/lintian-kwin-stereo3d15-final.log`.
- Five non-debug packages are in `/K3D/temp/sparky-os/pending-packages/`:
  `kwin-common`, `kwin-data`, `kwin-dev`, `kwin-wayland` and `libkwin6`, all
  at `4:6.7.4-2+stereo3d15`. The `.dsc`, Debian tarball, upstream tarball
  and upstream signature are in `/K3D/temp/sparky-os/repo/source/`.
- Local CI was run before and again before this hand-back with
  `kde-ci-local`, using a workspace-backed cache. The runnable qml-lint job
  passed and YAML reported only existing warnings. The SUSE build job cannot
  start here because the documented VM-only DRM probe requires missing
  `/dev/dri/card2`; the exact final result is in
  `evidence/kwin-configure-ci-before-review.log`. No KDE or freedesktop CI
  was triggered.

## One-value cleanup (2026-10-07, completed)

- Read the current `SOLUTIONS.md` and the latest AGENTS correction. The
  protocol and KWin interface now accept only none (0) and full side by side,
  left first (3), at the source resolution. The protocol no longer carries
  the duplicate `content_class` request. The X11 content-class property stays
  because the shared helper ABI and KWin's X11 metadata path still use it;
  Wayland content kind uses `wp_content_type_v1`.
- Plasma protocol commit `1c22f3b` removes the dead layouts and protocol class
  request. Helper commit `e85fbfe` restores the existing five-argument API,
  validates only the two layout values, writes the X11 class property, and
  maps Wayland class values to the existing content-type protocol.
- KWin commit `ba4f53ab37` removes the old layout handling from the scene,
  Wayland declaration, X11 layout reader, rules, and tests. Its X11 class
  reader remains intact. The commit message records why the earlier nine
  layouts were reduced to one format.
- The standalone helper source was rebuilt successfully in
  `evidence/stereo-declare-package-final.log`; generated protocol code no
  longer contains `set_content_class`. Plasma's installed protocol test
  suite passed all 125 tests in `evidence/protocol-one-value-final-tests.log`.
- The fresh KWin core rebuild completed through 118/118 with no compiler or
  linker error. The focused test targets then built through 7/7. Evidence is
  in `evidence/kwin-one-value-reuse-long2.log` and
  `evidence/kwin-one-value-test-build2.log`.
- The focused Wayland tests `kwin-testStereoContent` and
  `kwin-testVirtualStereoProtocol` passed. The focused X11 declaration,
  malformed-property and rule-precedence cases passed 8/8 in
  `evidence/kwin-one-value-stereo-tests.log`. The full X11 test binary also
  ran; its six unrelated decoration/glxgears failures are recorded in
  `evidence/kwin-one-value-test-fix-run.log`, while all stereo cases passed.
- The rule test exposed and fixed the sparse-value boundary: KConfigXT stores
  the two rule choices by index, while the wire value for full side by side is
  3. KWin commit `060866b448` maps that choice back to value 3. The source
  branch is clean at that commit.
- The local KWin CI XML job passed with exit 0 in
  `evidence/kwin-one-value-kde-ci-final.log`. The SUSE build and DRM jobs were
  not run because the local image lacks the documented VM-only DRM setup; no
  KDE or freedesktop CI was triggered.
- Mesa's tree has no source diff for this interface cleanup: its declarations
  already use only value 3 and never carry the class property. The existing
  `mesa/build-flush` configuration has no Meson tests defined; the final
  command returned 0 with that result in
  `evidence/mesa-one-value-meson-tests-final.log`. The accepted full Mesa test
  run remains in `evidence/mesa6-clean-build-with-layer.log`.
- The protocol and helper commits are `1c22f3b` and `e85fbfe`. The KWin
  cleanup commits are `ba4f53ab37` and `060866b448`. The next queue item is
  the Desktop Cube hooks.
- After verification, the superseded one-value KWin and protocol build trees
  were moved out of the workspace to `/tmp/mesa-stereo-builds.0bUYbB`; `/K3D`
  has 45 GB free. No container remains running.

## Coordinated package set (2026-10-07, blocked)

The helper source package `stereo-declare (1.0.1-1)` and the protocol source
package `plasma-wayland-protocols (1.21.0-1+stereo3d3)` were built, with their
source records and binary package records retained in the workspace. The
KWin source tree `kwin-stereo3d16-src3` contains the current one-value tree as
`debian/patches/stereo3d-all.patch`, and its package metadata is prepared for
`4:6.7.4-2+stereo3d16`, including `Breaks: libstereo-declare1 (<< 1.0.1-1)`.

The clean KWin package command was started in
`mesa-stereo-kwin-xwayland-build:3` as container
`mesa-stereo-kwin16-build`, with `--cpus 4`, `dpkg-buildpackage -d -b -uc -us
-j4`, and its full log in `evidence/kwin-stereo3d16-build.log`. The build
reached 82% without an error, but `/K3D` fell to 29 GB free. Per the disk
floor, the build was stopped and the container removed. No KWin binaries or
pending-package copies were made, and the combined installed-package proof
has not run. The package milestone remains unverified until the build can be
resumed after space is recovered.

The updated AGENTS note changed the floor to 20 GB on `/K3D` and 12 GB on
`/`. I resumed the package build after removing the superseded Mesa
source/build trees. The second run reached 67% in
`evidence/kwin-stereo3d16-build-resume.log`, then `/K3D` reached 19 GB free.
It was stopped again at that point. Its generated object tree was cleaned;
`/K3D` is now 21 GB free and `/` is 72 GB free. No KWin package or pending
copy exists, and the coordinated-package proof still has not run.

## CI diagnosis and coordinated packages (2026-10-07, resumed)

Read the latest AGENTS takeover note, branch history, packaging skill, and
SOLUTIONS.md. KWin is clean on `partner/kwin-one-value-v2` at `060866b448`;
its comparison base is `0ec08b5bd8`. The failed SUSE run lists exactly 64
test executables; their names are saved in `ci-diagnosis/failed-tests.txt`.
The previous SUSE build tree was deleted by the CI helper. A replacement
`--only-build` run is therefore running in the own-filesystem container
`mesa-stereo-kwin-ci-diagnosis`, as uid 1000 with `--cpus 2`, using KDE's
`suse-qt611` image `sha256:7b6056f40e88e8f4550fd0c50174b5dd370dc175554681447f3245ec44a0fa73`.
The scripts are in `ci-diagnosis/`; build output is
`evidence/kwin-ci-diagnosis-build.log`. No full CI rerun was started.

The pre-existing `mesa-stereo-kwin16-local` package build is still running
in `/kwin16` inside its container, without host bind mounts. Its progress
was 22 percent when inspected; no package-build success is claimed. Disk
checks showed 49 GB free on /K3D and 66 GB on /, above the current 20/12 GB
floors. Host load was about 45; the cause of the failed tests is not yet
verified. Next: run the failed tests serially on both commits with the same
SUSE dependencies, classify each result, then verify the coordinated
installed package set.

The original CI log shows a specific stale-test regression:
`kwin-testStereoDownscale` still supplies layouts 1 through 8. In its first
attempt, all five rows for layout 3 pass and all 35 removed-layout rows fail.
The test data is now limited to full side by side while preserving every
scale and both-eye reference-average assertions. This source change is
committed separately; it has not yet been verified by the new serial run.
The comparison still uses the unchanged cleanup commit `060866b448` and
its base before testing the fix, so the baseline evidence is retained.

The first replacement CI build was stopped after inspecting its CMake
cache: KDE's `--only-build` also selects `BUILD_TESTING=OFF`. Its log is
retained as `evidence/kwin-ci-diagnosis-build-testing-off.log`. The resumed
harness passes an explicit `-DBUILD_TESTING=ON` after the runner's defaults,
while retaining Debug, address sanitizer, coverage, and the two-CPU limit.
The original full CI log and its SHA-256 are now copied into
`evidence/kwin-one-value-original-ci.log` and `.sha256`, so the queued
coordinator rerun cannot overwrite this round's starting evidence.

The package tree also contains the isolated downscale test patch. The
container lacked the `quilt` command; `dpkg-source --before-build .` applied
it successfully instead. `evidence/kwin16-downscale-quilt.log` retains that
attempt and the source's applied-patch list was checked. Debian's package
build has `BUILD_TESTING=OFF`; the correction changes test data only.

The original retries also report Wayland socket lock failures before test
initialization, followed by an ASAN teardown crash in `Compositor::stop()`.
Those exact lines are retained in
`evidence/kwin-one-value-original-socket-locks.log`. The serial harness
cleans surviving own-build test clients and Xwayland between source variants
while keeping its image, dependency prefix, runtime path, and CPU limits
fixed. This prevents a timed-out first run from contaminating the base run.
No other container's processes are touched. The remaining failures still
need the requested serial comparison before classification.

Workspace scratch cleanup removed 164 superseded Mesa +stereo3d1 through
+stereo3d6 and KWin +stereo3d13/+stereo3d14 binary build artifacts, totalling
2,200,177,684 bytes (2.05 GiB). The exact files and sizes are recorded in
`evidence/superseded-binary-cleanup.tsv`. Repository Mesa +stereo3d8 and
KWin +stereo3d15 were checked present before the cleanup. Source records,
build records, and evidence remain. This does not complete the pending
coordinated package build or the serial test comparison.

The resumed CMake cache explicitly reports `BUILD_TESTING=ON`, Debug and
coverage, and `ctest -N` lists 174 tests, recorded in
`evidence/kwin-ci-diagnosis-test-config.log`. The harness scripts passed
shell/Python syntax checks inside the same SUSE container in
`evidence/kwin-ci-diagnosis-harness-syntax.log`. No runtime-test success is
claimed from these checks. The package source's applied patch list and
corrected data rows are in `evidence/kwin16-downscale-applied.log`.

The resumed comparison harness now rebuilds all targets after returning from
the accepted base to the corrected cleanup commit. Its earlier final step
built only the downscale executable, which could have left the base's KWin
library under test. Fetching the correction also uses the local branch name
rather than a short object ID. The updated script passed Python parsing in
the same SUSE container; the runtime comparison is still pending. Both
existing containers remain running with two and four CPU limits respectively.
The package build writes only to its container filesystem. Disk checks on
takeover found 25 GB free on /K3D and 54 GB on /, above the current floors.

KWin `4:6.7.4-2+stereo3d16` finished its binary build with return code 0,
including `dpkg-source --after-build` reversing both patches. The log and
status are `evidence/kwin-stereo3d16-local-build.log` and `.rc`. The build
used Qt `6.11.2+dfsg-5` and KConfig/CoreAddons `6.30.0-1`, recorded in
`evidence/kwin-stereo3d16-build-dependencies.tsv`. The first source-only
attempt lacked the upstream tarball in the container; after copying the
retained tarball and signature in, the source build returned 0. Both attempts
are retained, and the successful record is
`evidence/kwin-stereo3d16-local-source.log` and `.rc`.

The coordinated binary set, source packages, `.changes` and `.buildinfo`
files are now in `pending/`. That set is not yet verified or published.
The package build container was removed after its records were copied out.
A fresh `debian:testing` container is installing repository KWin +15 and
helper 1.0.0 for the upgrade proof. The prepared scripts install the new KWin
without requesting the helper explicitly, require apt to upgrade the helper,
and run the installed Haruna through installed headless KWin. Its captures
will check eye colours, frame numbers and unchanged controls against the
Haruna lane's reference clip. These runtime checks and lintian remain pending.
