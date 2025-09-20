![FFmpeg](https://img.shields.io/badge/-FFmpeg-4d853a?style=for-the-badge&logo=ffmpeg)
![GitHub release (latest by date)](https://img.shields.io/github/v/release/Revnoplex/vidtty?style=for-the-badge&logo=github)
# VidTTY
A command line based video player written in c that converts and plays videos using ascii characters

Usage of this program is featured [here](https://www.youtubeg.com/watch?v=OSnveMc77ss).

## Compatibility
This program has only been tested to work on some Linux environments listed below:

| OS Name                        | Architecture    |
|--------------------------------|-----------------|
| Arch Linux                     | amd64 (x86_64)  |
| Debian 12                      | amd64 (x86_64)  |
| Ubuntu 20.04                   | amd64 (x86_64)  |
| Ubuntu 22.04                   | amd64 (x86_64)  |
| Ubuntu 24.04                   | amd64 (x86_64)  |
| Termux 0.118.3 (on Android 16) | AArch64 (arm64) |
| Debian 12 (on Android 16)      | AArch64 (arm64) |
| Fedora 42                      | amd64 (x86_64)  |
| Debian 13                      | amd64 (x86_64)  |

The C program is yet to be tested on mac os and will likely not compile due to use of some linux specific headers.

### Supported Files
Most video file types should work as they are decoded with ffmpeg.


## Installing Build Dependencies
The general names build dependencies are listed in [build-dependencies-universal.txt](./build-dependencies-universal.txt). These will be different between systems and just using these names in package managers is not guaranteed to work. Instead use one of the `build-dependencies-$(OS_BASE).txt` files with your package manager for package names closer to your system with `OS_BASE` being the operating system closest to yours.

**Example**: on Arch based distros run
```
# pacman -S --needed - < build-dependencies-archlinux.txt
```

### Building
To build the c version after installing the nessarary dependencies run:
```sh
autoreconf --install
./configure
make
```

You can install the binary to your PATH with
```sh
sudo make install
```

## Usage
Some test files are provided to use. To run the program using e.g., one of the test files run:
```sh
$ ./vidtty fake-mr-beast-bad-apple-remix.mp4
```

## URL Support
To play a video url just pass it like a file:
```sh
$ ./vidtty https://revnoplex.xyz/media/downloads/videos/gamecube.mp4
```

You can play videos from youtube or other websites with yt-dlp (after installing [yt-dlp](https://github.com/yt-dlp/yt-dlp)) with:
```sh
$ ./vidtty $(yt-dlp -f b -g "ytsearch:bad apple tf2 cover")
```

## VIDTXT File Format
See [vidtxt.md](vidtxt.md) for more information.

