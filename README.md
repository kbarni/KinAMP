KinAMP3 - It really whips the llama's ass!
==========================================
<a href='https://ko-fi.com/E1E71RAR86' target='_blank'><img height='36' style='border:0px;height:36px;' src='https://storage.ko-fi.com/cdn/kofi3.png?v=6' border='0' alt='Buy Me a Coffee at ko-fi.com' /></a>

**Contents**

[Features](#features) - [Using the Native app](#native-app) - [Using the Koreader plugin](#koreader-plugin) - [Installation](#installation)

![Screenshot](assets/kinamp.png)

KinAMP is a native music and internet radio player developed for jailbroken Kindles. It runs as a standalone app or as a Koreader plugin, so you can keep listening while you read.

Features
--------

- Fully native fast (C++ and GTK2) interface
- Koreader plugin
- Simple to use
- Internet radio streams
- Interface optimized for eink displays (minimal redraws to save battery)
- Low power consumption (4-5% per hour with frontlight and display updates off)
- Fast access to Bluetooth and frontlight settings
- Background mode to continue listening while reading.
- Sleep mode for automatic stopping after a given time or at the end of the playlist
- Uses [miniaudio](https://github.com/mackron/miniaudio) library for decoding.
- Uses the integrated GStreamer library for output
- No other dependencies

**Supported audio formats:**
- MP3
- FLAC
- WAV
- OGG (Vorbis)
- AAC

KinAMP supports audio and radio metadata.

**IMPORTANT NOTE** This is the *Beta* version of KinAMP3, an important update of this application. **It might contain bugs. Please report any bugs, ideas, suggestions** by filing an issue.

Usage
------

### Native app

Start the native app using the **KinAMP** booklet from the library. You can also use **KUAL** launcher to start the app.

The controls are straightforward: Previous, Play/Pause, Stop and Next buttons, plus Repeat, Shuffle and a Volume slider. *Note: because of buffering between the decoder and playback, a volume change takes about 1 second to apply*.

![Button bar](assets/kinamp_button_bar.png)

The player has 2 modes: *music mode*, which plays local audio files, and *radio mode*, which plays internet radio stations. Switch between the two modes with the button in the lower left corner.

Music decoding itself uses relatively little power; on an e-ink device, the frontlight and screen redraws consume far more energy. To preserve battery, it's recommended to *turn off (or dim) the frontlight* and enable the *reduce screen refresh* option.

#### Using radio mode

Switch to radio mode using the button in the lower left corner. The playlist will be replaced with the radio station list.

To build your favorite radio list, press **Edit stations** in the lower button bar. The station manager lists your stations and lets you add one from the provided list (*over 45000 radio stations!*) or by typing a name and URL by hand. Playlist links (`.pls`, `.m3u`) are unwrapped to the stream address automatically.

##### Creating a station list manually

Create a file called `.kinamp_radio.txt` in a text editor, listing your preferred stations one per line in the format `Station name|URL`. Example

    Virgin Radio|http://icy.unitedradio.it/VirginHardRock.mp3

Copy this file to the `KinAMP` folder on the Kindle (`/mnt/us/KinAMP`).

#### Using background mode

- Start KinAMP and set up your desired playlist or radio station.
- Click the *Background* button (rectangle with an arrow, next to Close). KinAMP will close and playback will continue in the background.
- To stop background playback, launch the KinAMP booklet again.

### Koreader plugin

**Since version 3, the Koreader plugin offers the more streamlined experience for KinAMP.**

The KinAMP plugin is in the Koreader *Tools menu*. It will display a floating player as in the screenshot below:

![Koreader plugin](assets/kinamp-koreader.png)

Use the left button to open the playlist editor, and the right button for the radio stations. The plugin manages radio stations directly, and lets you save and load playlists on the fly.

The *hamburger menu* (top left corner) contains more advanced options: playback order, Bluetooth connection management and the about dialog. To completely shut down the player daemon, choose *Quit player*.

#### Quick access to KinAMP in Koreader

To quickly access KinAMP, assign it to a gesture: **Cog menu > Taps and Gestures > Gesture Manager > Tap corner**, then set the bottom-right corner to *KinAMP: show player*. This lets you open KinAMP with a single tap on the bottom-right corner of the screen.

**This is recommended if you plan to listen to music often while reading.**

Installation
------------

### Installing using `kpm`

Add the KindleTweaks repo to KPM, using `kterm` or an *SSH connection*:

    /var/local/kmc/bin/kpm add-repo https://kindletweaks.github.io/Repo/manifest.json

Then install KinAMP:

    /var/local/kmc/bin/kpm install kinamp

### Installing from the release

Download the [latest release](https://github.com/kbarni/KinAMP/releases) and unzip it to the root of the Kindle's storage. Start it from KUAL or from the home screen.

Kobo
----

KinAMP3 has experimental support for Kobo devices.

Kobo devices run the Koreader plugin only - there is no native app.

To install, download `kinamp-kobo.zip` from the release and unzip it to the root of the Kobo's storage. It installs:

```
.adds/koreader/kinamp/                   the player and its libraries
.adds/koreader/plugins/kinamp.koplugin/  the plugin
```

Both halves sit inside Koreader's own folder, so removing KinAMP is a matter of deleting
those two directories.

Audio goes out over Bluetooth, so **pair your headphones from the Kobo's own settings
before starting Koreader** - unlike on the Kindle, the plugin does not manage the Bluetooth
connection itself.

For more information about building and porting to other devices, see the [Hacking](HACKING.md) document.

License
-------

This program is free software: you can redistribute it and/or modify it under the terms of the **GNU General Public License** as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with this program. If not, see <http://www.gnu.org/licenses/>.
