# Third-party components

qBlank itself is under the GNU General Public License, version 3 or later;
see [LICENSE](LICENSE).

## Bundled in the source tree

**Dear ImGui** — `third_party/imgui` — Copyright (c) 2014-2026 Omar Cornut,
MIT License. Its own `LICENSE.txt` ships alongside the sources, and its
copyright notice must be preserved in any redistribution. MIT places no
condition the GPL does not already meet, so the combined work is distributed
under the GPL while Dear ImGui keeps its own notice and its own terms.

This is the only third-party code in the tree, and it is the only thing linked
into the executable beyond what Windows itself provides.

## Not bundled

**ffmpeg** is not shipped and not linked. Where it is used — recording, and
writing AVIF — qBlank starts it as a separate process and talks to it over a
pipe. It is fetched from upstream by the user, on the user's own terms, and it
stays that way on purpose: having it at all is the user's decision, without it
qBlank is a single 2 MB file, and a bundled copy would age — the download
fetches the current release build, a committed one would stay at the version it
was committed at. Invoking a program is not linking against it, so the two
remain separate works and neither licence reaches across.
