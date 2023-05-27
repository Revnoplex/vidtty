#!/usr/bin/env python3
import shutil
import signal
import struct
import subprocess
import traceback
from io import BytesIO
import time
from multiprocessing import Manager, Process, Queue, Value
import sys
import ctypes
import datetime
from types import TracebackType
from typing import Union
from PIL import Image
import cv2
import os


class OpenError(BaseException):
    pass


if sys.version_info[1] < 10:
    def exception_handler(exception_type: BaseException, exception: BaseException,
                          exception_traceback: Union[TracebackType, list]):
        if exception_type in [KeyboardInterrupt, EOFError, SystemExit]:
            return
        else:
            print("Traceback (most recent call last):")
            if isinstance(exception_traceback, TracebackType):
                traceback.print_tb(exception_traceback)
            else:
                traceback.print_list(exception_traceback)
            print(f'{exception_type.__name__}: {exception}', file=sys.stderr)
            exit(1)
else:
    def exception_handler(exception_type: type[BaseException], exception: BaseException,
                          exception_traceback: Union[TracebackType, list[traceback.FrameSummary]]):
        if exception_type in [KeyboardInterrupt, EOFError, SystemExit]:
            return
        else:
            print("Traceback (most recent call last):")
            if isinstance(exception_traceback, TracebackType):
                traceback.print_tb(exception_traceback)
            else:
                traceback.print_list(exception_traceback)
            print(f'{exception_type.__name__}: {exception}', file=sys.stderr)
            exit(1)


sys.excepthook = exception_handler


def dump_frames(video_filename: str, fps: float):
    terminal_lines, terminal_columns = (lambda px: (px.lines, px.columns))(os.get_terminal_size())
    to_write_name = f'{"".join(video_filename.rsplit(".", 1)[:-1])}.vidtxt'
    file_to_write = open(to_write_name, "wb")
    #                      0 to 5    6   7     8 to 11       12 to 15    16 to 23          24 to 32
    # layout of header: VIDTXT(str) NUL NUL {columns}(u32) {lines}(u32) {fps}(f64) {frame_start_address}(u64)
    # NUL to byte 0x3F
    #   29 to 63
    #                     0 to 63                      64 to x-1        x to EOF
    # full file layout: header to 0x3F (64 bytes), audio from 0x41, frames from value of x
    # x = frame_start_address
    if fps == float("inf"):
        print("033[1;31mFatal\033[0m: Cannot dump frames as the fps value cannot be stored as a 64 bit double")
        return
    # byte numbers:      0   1   2   3   4   5   6   7                            8 to 11
    initial_header = b'\x56\x49\x44\x54\x58\x54\x00\x00' + terminal_columns.to_bytes(4, "big", signed=False) + \
                     terminal_lines.to_bytes(4, "big", signed=False) + struct.pack("d", fps)
    #                                          12 to 15                 16 to 23
    #                           24 to 63
    mem_file = initial_header + b'\x00' * (64 - len(initial_header))
    if no_audio_required:
        # 24 to 32
        mem_file = mem_file[:24] + b'\x00' * 8 + mem_file[32:]
    else:
        print("Extracting audio from video file...")
        try:
            audio = subprocess.Popen(["ffmpeg", "-nostdin", "-i", video_file, "-loglevel", "panic", "-f", "mp3",
                                      "pipe:1"],
                                     stdout=subprocess.PIPE)
        except FileNotFoundError:
            print(
                f"\033[1;31mFatal\033[0m: ffmpeg executable not found. please make sure you install ffmpeg or make sure"
                f" the executable is in one of your PATH directories.", file=sys.stderr)
            return
        else:
            audio_bytes = BytesIO(audio.stdout.read()).read()
            # 24 to 32
            mem_file = mem_file[:24] + len(audio_bytes).to_bytes(8, "big", signed=False) + mem_file[32:]
            #                      64 to x-1
            mem_file = mem_file + audio_bytes
            # x = frame_start_address

    print(f"Writing to {to_write_name}...")
    file_to_write.write(mem_file)
    avg_interval_list = []
    current_frame = 0
    while True:
        start_time = datetime.datetime.now()
        if not video.isOpened():
            print("\033[1;31mFatal\033[0m: Failed to open video", file=sys.stderr)
            return
        average_interval = 1.0
        if len(avg_interval_list) > 0:
            average_interval = sum(avg_interval_list) / len(avg_interval_list)
        average_fps = 1 // average_interval
        time_left = average_interval * (total_frames - current_frame)
        print(f"\rRendering frame {current_frame} of {total_frames} "
              f"at a rate of {average_fps} fps. ETA: "
              f" {datetime.timedelta(seconds=time_left)}", end="")
        status, vid_frame = video.read()
        raw_frame = cv2.imencode(".jpg", vid_frame)[1].tobytes()
        frame = Image.open(BytesIO(raw_frame))
        resized_frame = frame.resize((terminal_columns, terminal_lines))
        img_data = resized_frame.getdata()
        ascii_gradients = [' ', '.', "'", '`', '^', '"', ',', ':', ';', 'I', 'l', '!', 'i', '>', '<', '~', '+',
                           '_', '-', '?', ']', '[', '}', '{', '1', ')', '(', '|', '\\', '/', 't', 'f', 'j', 'r',
                           'x', 'n', 'u', 'v', 'c', 'z', 'X', 'Y', 'U', 'J', 'C', 'L', 'Q', '0', 'O', 'Z', 'm',
                           'w', 'q', 'p', 'd', 'b', 'k', 'h', 'a', 'o', '*', '#', 'M', 'W', '&', '8', '%', 'B',
                           '@', '$']
        frame_width = resized_frame.width
        # frame_list: list[list[int, list[list[str, int]]]] = []
        frame_list = ""
        line = ""
        for index, pixel in enumerate(img_data):
            if index % frame_width:
                average_pixel_gradient = sum(pixel) / 3
                line += ascii_gradients[int(int(average_pixel_gradient) // (255 / (len(ascii_gradients) - 1)))]
            else:
                frame_list += line
                line = ""

        file_to_write.write(frame_list.encode("utf-8"))
        current_frame += 1
        duration = (datetime.datetime.now() - start_time).total_seconds()
        avg_interval_list.append(duration)
        if current_frame == total_frames:
            break
    file_to_write.close()


def render_frames(frames: Queue, dumped_frames: Value, dumping_interval: Value,
                  error: Queue, video_filename: str, total_frame_count: int):
    try:
        print("beginning to render frames...")
        current_frame = 0
        vid = cv2.VideoCapture(video_filename)
        avg_interval_list = []
        terminal_lines, terminal_columns = (lambda px: (px.lines, px.columns))(os.get_terminal_size())
        while True:
            start_time = datetime.datetime.now()
            if not vid.isOpened():
                print("\033[1;31mFatal\033[0m: Failed to open video", file=sys.stderr)
                return
            average_interval = 1.0
            if len(avg_interval_list) > 0:
                average_interval = sum(avg_interval_list)/len(avg_interval_list)
            dumping_interval.value = average_interval
            dumped_frames.value = current_frame
            status, vid_frame = vid.read()
            raw_frame = cv2.imencode(".jpg", vid_frame)[1].tobytes()
            frame = Image.open(BytesIO(raw_frame))
            resized_frame = frame.resize((terminal_columns, terminal_lines))

            img_data = resized_frame.getdata()
            ascii_gradients = [' ', '.', "'", '`', '^', '"', ',', ':', ';', 'I', 'l', '!', 'i', '>', '<', '~', '+',
                               '_', '-', '?', ']', '[', '}', '{', '1', ')', '(', '|', '\\', '/', 't', 'f', 'j', 'r',
                               'x', 'n', 'u', 'v', 'c', 'z', 'X', 'Y', 'U', 'J', 'C', 'L', 'Q', '0', 'O', 'Z', 'm',
                               'w', 'q', 'p', 'd', 'b', 'k', 'h', 'a', 'o', '*', '#', 'M', 'W', '&', '8', '%', 'B',
                               '@', '$']
            frame_width = resized_frame.width
            h_line_idx = 0
            frame_list: list[list[int, list[list[str, int]]]] = []
            frame_num = 0
            line = ""
            for index, pixel in enumerate(img_data):
                if index % frame_width:
                    average_pixel_gradient = sum(pixel) / 3
                    line += ascii_gradients[int(int(average_pixel_gradient) // (255 / (len(ascii_gradients) - 1)))]
                else:
                    if h_line_idx < terminal_lines - 1:
                        frame_list.append([h_line_idx, line])
                        frame_num += 1
                    h_line_idx += 1
                    line = ""

            frames.put((current_frame, frame_list))
            current_frame += 1
            duration = (datetime.datetime.now() - start_time).total_seconds()
            avg_interval_list.append(duration)
            if current_frame == total_frame_count:
                break
        exit()
    except Exception as e:
        error.put((type(e), e, traceback.extract_tb(e.__traceback__)))


lag = 0


def file_print_frames(filename):
    global no_audio_required
    with open(filename, "rb") as vidtxt_file:
        vidtxt_header = vidtxt_file.read(64)
        terminal_columns = int.from_bytes(vidtxt_header[8:12], "big", signed=False)
        terminal_lines = int.from_bytes(vidtxt_header[12:16], "big", signed=False)
        fps: float = struct.unpack("d", vidtxt_header[16:24])[0]
        audio_size = int.from_bytes(vidtxt_header[24:32], "big", signed=False)
        frames_start_from = 64 + audio_size
        vidtxt_file.seek(64, 0)
        if audio_size < 1:
            no_audio_required = True
        f_total_frames = \
            (os.stat(filename).st_size - frames_start_from) // ((terminal_columns - 1) * (terminal_lines - 1))
        vid_duration = (f_total_frames // fps) + (f_total_frames % fps) / fps
        if not no_audio_required:
            audio = subprocess.Popen(["ffmpeg", "-nostdin", "-i", "-", "-loglevel", "panic", "-f", "wav", "pipe:1"],
                                     stdin=vidtxt_file, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            audio_cmd = subprocess.Popen(["aplay", "--quiet"] if shutil.which("aplay") else ["play", "-q", "-V1", "-t",
                                                                                             "wav", "-"],
                                         stdin=audio.stdout)
    with open(filename, "rb") as vidtxt_file:
        vidtxt_file.seek(frames_start_from, 0)
        interval = 1 / fps
        std_scr = curses.initscr()
        curses.noecho()
        curses.cbreak()
        current_interval = interval
        global lag
        current_terminal_lines = os.get_terminal_size().lines
        current_terminal_columns = os.get_terminal_size().columns
        eof = False
        frame_number = 0
        displayed_since = datetime.datetime.now()
        try:
            while not eof:
                time_elapsed = datetime.datetime.now() - displayed_since
                calculated_frames = round(fps * time_elapsed.total_seconds())
                frames_behind = calculated_frames - frame_number
                start_time = datetime.datetime.now()
                pre_duration = (datetime.datetime.now() - start_time).total_seconds()
                if pre_duration >= current_interval:
                    lag += 1
                    current_interval = (pre_duration - current_interval) / lag
                std_scr.refresh()
                try:
                    iteration = 0
                    for line in range(terminal_lines - 1):
                        iteration += 1
                        line_contents = vidtxt_file.read(terminal_columns - 1)
                        if not len(line_contents):
                            eof = True
                        if line < current_terminal_lines - 1:
                            if terminal_columns > current_terminal_columns:
                                std_scr.addstr(line, 0, line_contents.decode("utf-8")[
                                                        :-(terminal_columns - current_terminal_columns)])
                            else:
                                std_scr.addstr(line, 0, line_contents.decode("utf-8"))
                    if debug_mode:
                        std_scr.addstr(terminal_lines - 1, 0,
                                       f'\rOutputted frame {frame_number}(approx. {calculated_frames})/{f_total_frames}'
                                       f' {time_elapsed}/{datetime.timedelta(seconds=vid_duration)} lagging '
                                       f'{frames_behind} frames behind')
                except _curses.error:
                    continue
                frame_number += 1
                duration = (datetime.datetime.now() - start_time).total_seconds()
                if duration < current_interval:
                    if frames_behind < 1:
                        time.sleep(current_interval - duration)
                    if frames_behind < 0:
                        time.sleep(current_interval - duration)
                else:
                    lag += 1
                    current_interval = (duration - current_interval) / lag
                if current_interval < interval:
                    current_interval = interval
        finally:
            curses.echo()
            curses.nocbreak()
            curses.endwin()


def print_frames(frames: Queue, dumped_frames: Value, dumping_interval: Value,
                 child_error: Queue):
    global no_audio_required
    print("Extracting audio from video file...")
    try:
        audio = subprocess.Popen(["ffmpeg", "-nostdin", "-i", video_file, "-loglevel", "panic", "-f", "wav",
                                  "pipe:1"],
                                 stdout=subprocess.PIPE)
    except FileNotFoundError:
        print(f"\033[1;31mError\033[0m: ffmpeg executable not found. please make sure you install ffmpeg or make sure "
              f"the executable is in one of your PATH directories.")
        exit()

    wait_for = video_duration
    interval = 1 / frame_rate

    while True:
        average_fps = 1 // dumping_interval.value
        time_left = dumping_interval.value * (total_frames-dumped_frames.value)
        if not time_left > wait_for:
            break
        if child_error.qsize() > 0:
            return child_error.get()
        print(f"\rDumping frame {dumped_frames.value} of {total_frames} "
              f"at a rate of {average_fps} fps. Video playback will approximately start in"
              f" {datetime.timedelta(seconds=(time_left-video_duration))}", end="")

    std_scr = curses.initscr()
    curses.noecho()
    curses.cbreak()
    audio_cmd = None
    if not no_audio_required:
        blank_sound = subprocess.Popen(["aplay", "--quiet"] if shutil.which("aplay") else ["play", "-q", "-V1", "-t",
                                                                                           "wav", "-"],
                                       stdin=subprocess.PIPE)
        blank_sound.communicate(input=b'RIFF%\x00\x00\x00WAVEfmt \x10\x00\x00\x00\x01\x00\x01\x00D\xac\x00\x00\x88X'
                                      b'\x01\x00\x02\x00\x10\x00datat\x00\x00\x00\x00')
        audio_cmd = subprocess.Popen(["aplay", "--quiet"] if shutil.which("aplay") else ["play", "-q", "-V1", "-t",
                                                                                         "wav", "-"],
                                     stdin=audio.stdout, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    current_interval = interval
    displayed_since = datetime.datetime.now()
    global lag

    try:
        for current_frame in range(total_frames):
            if child_error.qsize() > 0:
                os.kill(os.getpid(), signal.SIGINT)
            start_time = datetime.datetime.now()
            terminal_lines = os.get_terminal_size().lines
            if frames.qsize() < 1 or current_frame == 100:
                if not no_audio_required:
                    audio_cmd.send_signal(20)
                std_scr.clear()
                std_scr.addstr(0, 0, "Buffering...")
                std_scr.refresh()
                time.sleep(10)
                std_scr.clear()
                if not no_audio_required:
                    audio_cmd.send_signal(18)
                displayed_since + datetime.timedelta(seconds=10)
            frame_number, frame_list = frames.get(timeout=interval)
            time_elapsed = datetime.datetime.now() - displayed_since
            calculated_frames = round(frame_rate * time_elapsed.total_seconds())
            frames_behind = calculated_frames - frame_number
            pre_duration = (datetime.datetime.now() - start_time).total_seconds()
            if pre_duration >= current_interval:
                lag += 1
                current_interval = (pre_duration - current_interval) / lag
            std_scr.refresh()
            h_line_idx = 0

            try:
                for frame in frame_list:
                    if frame[0] < terminal_lines - 1:
                        std_scr.addstr(frame[0], 0, frame[1])
                    h_line_idx += 1
                if debug_mode:
                    std_scr.addstr(terminal_lines - 1, 0,
                                   f'\rOutputted frame {frame_number}(approx. {calculated_frames})/{total_frames} '
                                   f'{time_elapsed}/{datetime.timedelta(seconds=video_duration)} lagging '
                                   f'{frames_behind} frames behind')
            except _curses.error:
                continue
            duration = (datetime.datetime.now() - start_time).total_seconds()
            if duration < current_interval:
                if frames_behind < 1:
                    time.sleep(current_interval - duration)
                if frames_behind < 0:
                    time.sleep(current_interval - duration)
            else:
                lag += 1
                current_interval = (duration - current_interval) / lag
            if current_interval < interval:
                current_interval = interval
        std_scr.addstr(0, 0, "Press Ctrl-C to exit")
    finally:
        curses.echo()
        curses.nocbreak()
        curses.endwin()
        if child_error.qsize() > 0:
            return child_error.get()


if __name__ == '__main__':
    print("vidtty v1.1.0")
    if sys.platform not in ["linux", "darwin"]:
        print("\033[1;33mWarning\033[0m: This version of vidtty has only been tested to on Unix based OSes such as"
              " Linux or MacOS. \nIf you are running this program in Windows, using cygwin is recommended. "
              "\nYou may also need to install the curses module manually."
              "\nThe behaviour of the program could be unpredictable.")
        input("Press enter to continue...")
    try:
        import curses
        import _curses
    except ModuleNotFoundError:
        curses = None
        _curses = None
        print(f"\033[1;31mFatal\033[0m: curses module not found. please make sure you have the package installed.")
        exit(1)
    video_file = sys.argv[-1]
    options = sys.argv[1:-1]
    if set(options + [video_file]).intersection({"--help", "-h"}) or len(sys.argv) < 2:
        print("\033[1mHelp Menu\033[0m")
        print("Usage vidtty [OPTIONS] FILE")
        print("-h --help\tHelp - displays this menu")
        print("-t --tty\tTTY - Send output to another file or tty instead of the default stdout")
        print("-b --debug-mode\tDebug Mode - Extra information will show at the bottom of the screen when playing")
        print("-m --no-audio\tNo Audio - Play or save video without any audio. Avoids loading up any audio modules")
        print("-d --dump\tDump - Convert the video to a instantly playable vidtxt file")
        video_file = None
        exit(1)
    if len(sys.argv) < 2:
        print("No video file specified. Please specify one. mp4 files works the best")
        video_file = None
        exit(1)

    if set(options).intersection({"--tty", "-t"}) and len(options) > 1:
        if not options[0].startswith("-"):
            video_file = sys.argv[-3]
            options = sys.argv[1:-3] + sys.argv[-2:]
        if options.index("-t")+1 < len(options):
            tty = options[options.index("-t")+1]
        else:
            tty = "/dev/stdout"
        try:
            open(tty, "rb").close()
            open(tty, "wb").close()
        except FileNotFoundError:
            print(f"Output pipe \"{tty}\" not found!")
            exit(1)
        except PermissionError:
            print(f"Need permission to write to \"{tty}\"\nRunning sudo...")
            os.system(f"sudo chown {os.getuid()} {tty}")
            os.system(f"chmod 600 {tty}")
        print("Running on another terminal session...")
        with open(tty, 'rb') as inf, open(tty, 'wb') as outf:
            os.dup2(inf.fileno(), 0)
            os.dup2(outf.fileno(), 1)
            os.dup2(outf.fileno(), 2)
        os.environ['TERM'] = 'linux'
    if options:
        debug_mode = bool(set(options).intersection({"--debug", "-b"}))
    else:
        debug_mode = False
    if set(options).intersection({"--no-audio", "-m"}):
        no_audio_required = True
        if len(sys.argv) > 2:
            pass
        else:
            print("No video file specified. Please specify one. mp4 files works the best")
            video_file = None
            exit(1)
    else:
        no_audio_required = False
    if not os.path.exists(video_file):
        print(f"File \"{video_file}\" not found!")
        exit(1)
    with open(video_file, "rb") as vidtxt_check:
        first_8 = vidtxt_check.read(8)
    if video_file.endswith(".vidtxt") or first_8 == b'VIDTXT\x00\x00':
        file_print_frames(video_file)
    else:
        video = cv2.VideoCapture(video_file)
        total_frames = int(video.get(cv2.CAP_PROP_FRAME_COUNT))
        frame_rate = video.get(cv2.CAP_PROP_FPS)
        frame_rate = 30.0 if not frame_rate else frame_rate
        video_duration = (total_frames // frame_rate) + (total_frames % frame_rate) / frame_rate
        global_interval = (1 / frame_rate)
        if set(options).intersection({"--dump", "-d"}):
            dump_frames(video_file, frame_rate)
        else:
            manager = Manager()
            queue = manager.Queue()
            shared_dumped_frames = Value(ctypes.c_int, 0)
            shared_dumping_interval = Value(ctypes.c_float, 1)
            shared_child_error = manager.Queue()
            p1 = Process(target=render_frames, args=(queue, shared_dumped_frames, shared_dumping_interval,
                                                     shared_child_error, video_file, total_frames,),
                         name="Frame Renderer")
            try:
                p2 = Process(target=print_frames, args=(queue, shared_dumped_frames, shared_dumping_interval,
                                                        shared_child_error))
                p1.exception = exception_handler
                p1.start()
                child_error_state = print_frames(queue, shared_dumped_frames, shared_dumping_interval,
                                                 shared_child_error)
                if child_error_state:
                    exception_handler(*child_error_state)
            finally:
                p1.terminate()
