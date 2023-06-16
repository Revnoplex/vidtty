![Python](https://img.shields.io/badge/-Python-14354C?style=for-the-badge&logo=python&logoColor=FFD43B)
![FFmpeg](https://img.shields.io/badge/-FFmpeg-4d853a?style=for-the-badge&logo=ffmpeg)
![open-cv](https://img.shields.io/badge/-OpenCV-000000?style=for-the-badge&logo=opencv)
# Vidtty
A command line based video player written in python that converts and plays videos using ascii characters

This program is featured in one of my [YouTube videos](https://www.youtube.com/watch?v=OSnveMc77ss).

## Compatibility
This program has only been tested to work on some Linux and macOS environments listed below:

| OS Name                        | Architecture   | Python Versions          |
|--------------------------------|----------------|--------------------------|
| Arch Linux                     | amd64 (x86_64) | python 3.10, python 3.11 |
| Ubuntu 20.04                   | amd64 (x86_64) | python 3.8, python 3.10  |
| macOS Big Sur                  | amd64 (x86_64) | python 3.10, python3.11  |
| Debian 11 (on Chrome OS)       | amd64 (x86_64) | python 3.9               |
| Termux 0.118.0 (on Android 13) | AArch64 (arm64)| python 3.11              |


The program has been tested to work on python 3.8 and later, there may be some issues installing pygame in python 3.11, which is why it is recommended to use python 3.10 for the best compatibility.

It might work on Windows if you use cygwin, Otherwise you will manually need to install a curses package, and you may run into environment related errors such as the behaviour of shared memory objects.

Only mp4 files have been tested and most are guaranteed to work. Opencv and/or ffmpeg and/or pygame might not support some formats


## Installing Dependencies
This program requires both python packages and other packages than can't be installed with pip alone.

### Installing Python dependencies
Just install the packages in the [requirements.txt](./requirements.txt) file
```sh
python3 -m pip install -r requirements.txt
```

### Installing Other Dependencies
The other dependencies are listed in the [non-pip-requirements.txt](./non-pip-requirements.txt) file. You should be able to install them using your package manager but the names for the opencv package may vary. 

e.g., The package name for opencv in the archlinux repositories is `python-opencv` while the package name in the ubuntu repositories is `python3-opencv`

If you can't seem to install opencv, you can install the unofficial python package:
```sh
python3 -m pip install opencv-python
```

## Usage
Some test files are provided to use. To run the program using e.g., one of the test files run:
```sh
./vidtty.py ./fake-mr-beast-bad-apple-remix.mp4
```

