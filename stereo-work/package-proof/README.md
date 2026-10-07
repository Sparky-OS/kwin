The installed-package proof reuses the Haruna lane's clips and the players
lane's ScreenShot2 collector and frame-marker reader:

- `players-stereo/.scratch/run/qt-capture` and `layers.py`
- `haruna-stereo/work/clips/sei-sbs-L/sei-sbs-L.mp4` and `.ass`
- `haruna-stereo/work/clips/2d/2d.mp4` and `.ass`

Both lanes are under `/K3D/temp/partners/queue/`. Copy the collector and reader
beside these scripts, and the clip pairs into `clips/stereo/stereo.*` and
`clips/mono/mono.*`. Separate folders prevent Haruna's next-file feature from
changing the control to stereo during capture.

Run `install.sh`, `check-packages.sh` and `upgrade.sh` as root inside a fresh
`debian:testing` container. Mount this workspace at `/workspace` read-only
and the development repository at `/repo` read-only. They write records only
inside the container. The upgrade requests KWin and the protocol package;
the helper must be pulled in by apt.

Run the installed stack as uid 1000 with `--cpus 4`, `--cap-add SYS_NICE`,
`--device /dev/dri/renderD128`, `--group-add 125` and the workspace writable.
Use `dbus-run-session -- python3 /workspace/package-proof/run.py stereo`,
then `analyze.py stereo`; repeat both commands for `mono`. The stereo
analysis with `--swap` must return 1 at the eye-colour assertion.

The collector's mean-colour classifier samples the frame-number subtitle;
its result is unsuitable for this clip. Transport failures return 2 or more.
The proof instead requires six actual images, the expected stereo width,
red/blue eye colours, matching video and subtitle frame numbers, and no
different pixels outside the video. Marker rectangles may differ by one
filtered boundary pixel; their union defines the video rectangle.
