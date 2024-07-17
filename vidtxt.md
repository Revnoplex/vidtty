# VIDTXT File Format

## Background
The `vidtxt` file format is a format created for this program for rendering video as ASCII art and storing it in a file 
along with its audio. This can be convenient when converting a video takes a long time, so then you only have to render it 
once and then play the video from the converted file. When using the `-d` argument (e.g. `./vidtty -d fake-mr-beast-bad-apple-remix.mp4`) the program 
will convert the video to ASCII art and store it in a file with the `.vidtxt` extension which can then be played 
instantly by the program. Pre-rendering a video is no longer needed that much anymore as the performance of the 
conversion process for this program has been improved drastically since the file format was created.

## File Structure
The file is separated into 3 different parts

Note: refer to the file header information table for more info on `audio_size`

| File Part            | From (bytes)                            | To (bytes) (Including)                  |
|----------------------|-----------------------------------------|-----------------------------------------|
| Header               | 0 (0x00)                                | 63 (0x3F)                               |
| Audio                | 64 (0x40)                               | `63 + audio_size` (`0x3F + audio_size`) |
| Video (as ASCII art) | `64 + audio_size` (`0x40 + audio_size`) | end of file                             |


### File Header
The structure of the header is as follows:

| Name         | Description                                                                                              | Data Type               | Byte Order | From (bytes) (Decimal) | From (bytes) (Hexadecimal) | To (bytes) (Decimal) (Including) | To (bytes) (Hexadecimal) (Including) |
|--------------|----------------------------------------------------------------------------------------------------------|-------------------------|------------|------------------------|----------------------------|----------------------------------|--------------------------------------|
| Identifier   | Helps identify the file as a VIDTXT file. Value: `VIDTXT`                                                | String                  | N/A        | 0                      | 0x00                       | 5                                | 0x05                                 |
| NULL         | 2 Null bytes                                                                                             | N/A                     | N/A        | 6                      | 0x06                       | 7                                | 0x07                                 |
| `columns`    | The columns or width of the video. This is inherited from the terminal size when the video was converted | 32-bit unsigned integer | Big endian | 8                      | 0x08                       | 11                               | 0x0B                                 |
| `lines`      | The lines or height of the video. This is inherited from the terminal size when the video was converted  | 32-bit unsigned integer | Big endian | 12                     | 0x0C                       | 15                               | 0x0F                                 |
| `fps`        | The frames per second or frame rate to play the video at. This is inherited from the original video      | Double (64-bit)         | Big endian | 16                     | 0x10                       | 23                               | 0x17                                 |
| `audio_size` | The size of the audio in bytes (if any). Defaults to a value of 0 if no audio                            | 64-bit unsigned integer | Big endian | 24                     | 0x18                       | 31                               | 0x1F                                 |
| NULL         | 32 Null bytes                                                                                            | N/A                     | N/A        | 32                     | 0x20                       | 63                               | 0x3F                                 |

### Audio
The audio is extracted from the original video and converted to a mp3 container and stored after the header from byte 64 (0x40) to byte `63 + audio_size` (`0x3F + audio_size`) (including) before the video. If there is no audio (e.g. The original file never had audio or the `-m` option was specified), then this section won't take up any space in the file and `audio_size` will be set to a value of 0.

### Video
Each frame of the video is stored as ASCII art in plain text with each frame being `columns * lines` bytes long.

