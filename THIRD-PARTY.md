# Third-party components

qBlank itself is under the GNU General Public License, version 3 or later;
see [LICENSE](LICENSE).

## Bundled in the source tree

**Dear ImGui** — `third_party/imgui` — Copyright (c) 2014-2026 Omar Cornut,
MIT License. Its own `LICENSE.txt` ships alongside the sources, and its
copyright notice must be preserved in any redistribution. MIT places no
condition the GPL does not already meet, so the combined work is distributed
under the GPL while Dear ImGui keeps its own notice and its own terms.

This is the only third-party code linked into the executable beyond what
Windows itself provides.

**FFmpeg headers** — `third_party/ffmpeg/include` — the public headers of
libavcodec, libavformat, libavutil and libswresample from FFmpeg 9.0,
unchanged, GNU Lesser General Public License 2.1 or later; the notices are in
the files. They describe the libraries below and contain no code that ends up
in the executable beyond inline helpers. LGPL 2.1 or later is compatible with
GPL 3.

## Not bundled

**FFmpeg libraries** are not shipped. Where they are used — recording,
remuxing and writing AVIF — qBlank loads `avcodec`, `avformat`, `avutil` and
`swresample` at runtime from the `ffmpeg` folder beside it. The user fetches
them from upstream, from the shared build at
[GyanD/codexffmpeg](https://github.com/GyanD/codexffmpeg), and it stays that
way on purpose: having them at all is the user's decision, without them qBlank
is a single 3 MB file, and a bundled copy would age. That build is configured
with `--enable-gpl --enable-version3`, so it is under GPL 3, the same terms as
qBlank, and the program running with it is a GPL 3 work.
