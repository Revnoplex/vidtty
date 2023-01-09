# ascii-video
Displays videos in a terminal/tty using ascii characters.

This program is featured in one of my YouTube videos.

## Compatibility
This program has only been tested to work on Linux. It should work on Mac OS and possibly on windows using cygwin. Otherwise you will manually need to install a curses package

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
./ascii-video ./fake-mr-beast-bad-apple-remix.mp4
```

