# MinimalVideoViewer

A tiny looping video player for Windows, built on libmpv (hardware decoding, GPU scaling).

![MinimalVideoViewer playing a video with the control bar and subtitles visible](docs/screenshot.jpg)

**Scroll to zoom in at the mouse pointer.** The zoom stays put while the video keeps looping.

![Scrolling the mouse wheel zooms in on the bunny's face and back out](docs/zoom.gif)

| VCR-style fast forward and rewind | Right-click menu: audio, subtitles and more |
| --- | --- |
| ![Fast forwarding at 4x, shown as "FF 4×" in the control bar](docs/fast-forward.jpg) | ![Right-click menu with the Subtitles submenu open](docs/menu.jpg) |

## Build

Run `build.bat` (needs Visual Studio C++ Build Tools). It downloads libmpv on first run and
produces `build\MinimalVideoViewer.exe` + `build\libmpv-2.dll` — keep those two files together.

## Use

`MinimalVideoViewer.exe video.mp4`, drag a file onto the window, or start it without arguments to pick a file.
Videos always loop; zoom and pan are kept across loops.

Moving the mouse shows a control bar (progress bar, rewind, play/pause, fast forward) that hides
after 5 seconds without mouse movement. Rewind and fast forward work like a VCR shuttle: smooth,
silent playback at 2x, and each extra press goes faster (4x, 8x, 16x). Rewind plays truly backwards
frame by frame and stops at the start; press play to return to normal playback.

| Input | Action |
| --- | --- |
| Mouse wheel | Zoom in/out at the cursor |
| Left drag | Pan (when zoomed) |
| Click/drag progress bar | Seek |
| Right-click | Menu: audio track, subtitles (incl. loading a subtitle file), jump distance, open, fullscreen, reset zoom, quit |
| Drop a subtitle file | Load and show it (.srt, .ass, .ssa, .vtt, .sub, .idx, .sup) |
| R | Reset zoom |
| Double-click / F / F11 | Fullscreen |
| Space / K | Play/pause (also ends rewind/fast forward) |
| J / L | Rewind / fast forward (press again: faster) |
| Left / Right | Jump back/forward (default 5 s; set it in the right-click menu, it is remembered) |
| Up / Down, M | Volume, mute (shown briefly at the top) |
| O | Open file |
| Esc | Leave fullscreen |
| Q | Quit |

## License

The code in this repository is MIT licensed (see [LICENSE](LICENSE)).

It uses [libmpv](https://mpv.io/), which is downloaded by `build.bat` and not part of this
repository. libmpv is licensed under the GPL, so if you distribute the built program together
with `libmpv-2.dll`, that bundle must follow the GPL terms (include the license text and make the
source available).

The preview images show *Big Buck Bunny*, (c) copyright 2008, Blender Foundation /
[www.bigbuckbunny.org](https://www.bigbuckbunny.org), licensed under
[CC BY 3.0](https://creativecommons.org/licenses/by/3.0/). The subtitles in the images were added
for the demo.
