# Building and modifying KinAMP

Building
--------

Install the kox toolchain, clone the GIT repo and adapt it to your paths in the provided armhf-toolchain.cmake file.

You will need to install the *Gstreamer 0.10* header files to the `usr/include` folder of the sysroot of the toolchain. Grab them from the `libgstreamer-0.10-dev` (or similar) package of any old linux distro (doesn't have to be arm architecture package).

```
git clone --recurse-submodules https://github.com/kbarni/KinAMP
cd Kinamp
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=armhf-toolchain.cmake
```

Hacking and porting to other devices
------------------------------------

KinAMP - especially the Koreader plugin - should be easy to port to other e-book readers, like the Kobo.

The `music_backend.cpp` manages all the audio-related stuff. It has two classes: the `Decoder` handles the decoding and creates a FIFO stream on `/tmp/kinamp_audio_pipe` in 16 bit signed integer type (this is the only format Kindle's decoder accepts). 

The `MusicBackend` class streams this to GStreamer as a `fdsource` and handles play, pause etc.

KinAMP has two frontends: a minimal daemon in `cli_player.cpp` and a GTK 2 interface in `music_player.cpp` for the native app (Kindles ship with this old UI toolkit). The good news is that `cli_player.cpp` has no special dependencies, it will build on any C++ compiler. And the koreader plugin relies on this daemon.

Porting the player for other devices would require only to change the `MusicBackend` class. It will compile with minimal changes on other systems with Gstreamer installed (with minimal changes, like replacing `mixersink` with `autosink` or similar - check the available plugins and formats). Replacing Gstreamer with ALSA shouldn't pose problems either, as it should be able to play this stream.

So the easiest way to port this app to another e-reader device should be:
- Set up a cross-compile toolchain
- Check the audio library on the device (GStreamer, ALSA...)
- Adapt the `MusicBackend` class - this is the only critical part - see explanation above.
- Rebuild `KinAMP-minimal` binary (remove the `KinAMP` binary related stuff in `CMakeLists.txt`)
- Adapt the `startkinamp-koreader.sh` script
- Copy the `kinamp.koplugin` to the `koreader/plugins` folder, the `KinAMP-minimal`, `startkinamp-koreader.sh` and `allStations.json` to `koreader/kinamp`
- Enjoy!

If the device ships with GTK2 library, the native app should compile too (I managed to compile it for Linux desktop).