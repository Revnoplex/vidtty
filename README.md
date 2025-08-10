![Python](https://img.shields.io/badge/-Python-14354C?style=for-the-badge&logo=python&logoColor=FFD43B)
![FFmpeg](https://img.shields.io/badge/-FFmpeg-4d853a?style=for-the-badge&logo=ffmpeg)
![GitHub release (latest by date)](https://img.shields.io/github/v/release/Revnoplex/vidtty?style=for-the-badge&logo=github)
# VidTTY
A command line based video player written in python (rewriting in c) that converts and plays videos using ascii characters

Example usage of this program is featured [here](https://www.youtube.com/watch?v=OSnveMc77ss).

## Compatibility
This program has only been tested to work on some Linux and macOS environments listed below:

### Python Version
| OS Name                        | Architecture    | Python Versions          |
|--------------------------------|-----------------|--------------------------|
| Arch Linux                     | amd64 (x86_64)  | python 3.10, python 3.11 |
| Ubuntu 20.04                   | amd64 (x86_64)  | python 3.8, python 3.10  |
| macOS Big Sur                  | amd64 (x86_64)  | python 3.10, python3.11  |
| Debian 11 (on Chrome OS)       | amd64 (x86_64)  | python 3.9               |
| Termux 0.118.0 (on Android 13) | AArch64 (arm64) | python 3.11              |

### C version
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

The python version of the program has been tested to work on python 3.8 and later.

Neither Program currently has been testet to work on Windows.

### Supported Files
Most video file types should work as they are decoded with ffmpeg.

The C version only currently supports playing .vidtxt files and has limited .vidtxt file generation, so you will have to generate using the python version with
```sh
$ ./vidtty.py -d example.mp4
```

## Installing Dependencies (Python Version)
This program requires both python packages and binary packages than can't be installed with pip alone.

### Installing Python dependencies
Just install the packages in the [requirements.txt](./requirements.txt) file
```sh
$ python3 -m pip install -r requirements.txt
```

### Installing Binary Dependencies
The binary dependencies are listed in the [binary-requirements.txt](./binary-requirements.txt) file. You should be able to install them using your package manager.

## Installing Build Dependencies (C version)
The general names build dependencies are listed in [build-dependencies-universal.txt](./build-dependencies-universal.txt). These will be different between systems and just using these names in package managers is not guaranteed to work. Instead use one of the `build-dependencies-$(OS_BASE).txt` files with your package manager for package names closer to your system with `OS_BASE` being the operating system closest to yours.

**Example**: on Arch based distros run
```
# pacman -S --needed - < build-dependencies-archlinux.txt
```

### Building (C version)
To build the c version after installing the nessarary dependencies run:
```sh
autoreconf --install
./configure
make
```

## Usage (Python version)
Some test files are provided to use. To run the program using e.g., one of the test files run:

```sh
$ ./vidtty.py ./fake-mr-beast-bad-apple-remix.mp4
```
## Usage (C version)
To play a vidtxt file (after generating one with the python version as stated [here](#supported-files)), run:
```sh
$ ./vidtty example.vidtxt
```

## VIDTXT File Format
See [vidtxt.md](vidtxt.md) for more information

