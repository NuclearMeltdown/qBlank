# FFmpeg headers

The public headers of libavcodec, libavformat, libavutil and libswresample from
FFmpeg 9.0 (commit d32b387f2b), taken unchanged from the `9.0` shared build in
[GyanD/codexffmpeg](https://github.com/GyanD/codexffmpeg/releases/tag/9.0).
Their licence notices are in the files.

qBlank links nothing against them. It loads the four DLLs at runtime from the
`ffmpeg` folder next to the exe, which the downloader fills from the newest 9.x
release, and checks their major versions against these headers.
