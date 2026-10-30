# libudfread 1.2.0 (vendored)

UDF reader from VideoLAN, LGPL-2.1-or-later (see COPYING). Playser uses it
to read a disc's catalogue files (stream sizes, playlists, IFOs, metadata)
straight from the drive, without mounting it.

Source: https://download.videolan.org/pub/videolan/libudfread/libudfread-1.2.0.tar.xz
(sha256 bb477cbd4cfbfc7787d9d05b71ee5e70430f5cfebf1297497f7e83547958050f),
`src/` unchanged except the version files left out. Built as a static
library by `app/CMakeLists.txt`.
