#!/usr/bin/env python3
import signal
import subprocess
import traceback
from io import BytesIO, FileIO
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

os.environ['PYGAME_HIDE_SUPPORT_PROMPT'] = "hide"


def dump_frames(video_filename: str):
    terminal_lines, terminal_columns = (lambda px: (px.lines, px.columns))(os.get_terminal_size())
    to_write_name = f'{"".join(video_filename.rsplit(".", 1)[:-1])}.vidtxt'
    file_to_write = open(to_write_name, "wb")
    #                      0 to 5    6     7 to 10     11    12 to 15   16           17 to 25             26 to 63
    # layout of header: VIDTXT(str) NUL {columns}(u32) NUL {lines}(u32) NUL {frame_start_address}(u64) NUL to byte 0x3F

    #                     0 to 63                     64        65 to x-2        x - 1           x to EOF
    # full file layout: header to 0x3F (64 bytes), null byte, audio from 0x41, null byte, frames from value of x
    # x = frame_start_address

    # byte numbers:      0   1   2   3   4   5   6                                7 to 10
    initial_header = b'\x56\x49\x44\x54\x58\x54\x00' + terminal_columns.to_bytes(4, "big", signed=False) + \
                     b'\x00' + terminal_lines.to_bytes(4, "big", signed=False)
    #                    11                           12 to 15
    #                           16 to 63
    mem_file = initial_header + b'\x00' * (64 - len(initial_header))
    # todo: add frame rate and total frames to header
    print("Extracting audio from video file...")
    try:
        audio = subprocess.Popen(["ffmpeg", "-i", video_file, "-loglevel", "panic", "-f", "mp3",
                                  "pipe:1"],
                                 stdout=subprocess.PIPE)
    except FileNotFoundError:
        print(
            f"\033[1;31mFatal\033[0m: ffmpeg executable not found. please make sure you install ffmpeg or make sure "
            f"the executable is in one of your PATH directories.", file=sys.stderr)
        raise Exception
    else:
        if no_audio_required:
            # 17 to 25
            mem_file = mem_file[:17] + b'\x00'*8 + mem_file[25:]
        else:
            audio_bytes = BytesIO(audio.stdout.read()).read()
            # 17 to 25
            mem_file = mem_file[:17] + (64 + len(audio_bytes) + 2).to_bytes(8, "big", signed=False) + mem_file[25:]
            #                        64     64 to x-2        x-1
            mem_file = mem_file + b'\x00' + audio_bytes + b'\x00'
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
            line = ""
            for index, pixel in enumerate(img_data):
                if index % frame_width:
                    average_pixel_gradient = sum(pixel) / 3
                    line += ascii_gradients[int(int(average_pixel_gradient) // (255 / (len(ascii_gradients) - 1)))]
                else:
                    if h_line_idx < terminal_lines - 1:
                        frame_list.append([h_line_idx, line])
                    h_line_idx += 1
                    line = ""

            frames.put(frame_list)
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
    pygame.init()
    with open(filename, "rb") as vidtxt_file:
        vidtxt_header = vidtxt_file.read(64)
        terminal_columns = int.from_bytes(vidtxt_header[7:11], "big", signed=False)
        terminal_lines = int.from_bytes(vidtxt_header[12:16], "big", signed=False)
        print(terminal_columns)
        print(terminal_lines)
        frames_start_from = int.from_bytes(vidtxt_header[17:25], "big", signed=False)
        audio_ends_at = frames_start_from - 2
        vidtxt_file.seek(65, 0)
        pygame.mixer.music.load(BytesIO(vidtxt_file.read(audio_ends_at - 65)))
    pygame.mixer.music.play()
    with open(filename, "rb") as vidtxt_file:
        vidtxt_file.seek(frames_start_from, 0)
        current_terminal_lines = os.get_terminal_size().lines
        current_terminal_columns = os.get_terminal_size().columns
        for line in range(terminal_lines - 2):
            if terminal_columns > current_terminal_columns:
                print(vidtxt_file.read(terminal_columns - 1).decode("utf-8")[
                      :-(terminal_columns - current_terminal_columns)])
            else:
                print(vidtxt_file.read(terminal_columns - 1).decode("utf-8"))
        interval = 1 / 30
        std_scr = curses.initscr()
        curses.noecho()
        curses.cbreak()
        current_interval = interval
        global lag
        current_terminal_lines = os.get_terminal_size().lines
        current_terminal_columns = os.get_terminal_size().columns
        try:
            while True:
                start_time = datetime.datetime.now()
                pre_duration = (datetime.datetime.now() - start_time).total_seconds()
                if pre_duration >= current_interval:
                    lag += 1
                    current_interval = (pre_duration - current_interval) / lag
                std_scr.refresh()
                try:
                    for line in range(terminal_lines - 1):
                        if line < current_terminal_lines - 1:
                            if terminal_columns > current_terminal_columns:
                                std_scr.addstr(line, 0, vidtxt_file.read(terminal_columns - 1).decode("utf-8")[
                                                        :-(terminal_columns - current_terminal_columns)])
                            else:
                                std_scr.addstr(line, 0, vidtxt_file.read(terminal_columns - 1).decode("utf-8"))
                        else:
                            vidtxt_file.read(terminal_columns - 1).decode("utf-8")
                except _curses.error:
                    continue
                duration = (datetime.datetime.now() - start_time).total_seconds()
                if duration < current_interval:
                    time.sleep(current_interval - duration)
                else:
                    lag += 1
                    current_interval = (duration - current_interval) / lag
                if current_interval < interval:
                    current_interval = interval
            os.kill(os.getpid(), signal.SIGINT)
        finally:
            curses.echo()
            curses.nocbreak()
            curses.endwin()
    time.sleep(60)


def print_frames(frames: Queue, dumped_frames: Value, dumping_interval: Value,
                 child_error: Queue):
    global no_audio_required
    if not no_audio_required:
        pygame.init()
    print("Extracting audio from video file...")
    try:
        audio = subprocess.Popen(["ffmpeg", "-i", video_file, "-loglevel", "panic", "-f", "mp3",
                                  "pipe:1"],
                                 stdout=subprocess.PIPE)
    except FileNotFoundError:
        print(f"\033[1;31mError\033[0m: ffmpeg executable not found. please make sure you install ffmpeg or make sure "
              f"the executable is in one of your PATH directories.")
        exit()
    else:
        if not no_audio_required:
            try:
                pygame.mixer.music.load(BytesIO(audio.stdout.read()))
            except pygame.error:
                print("\033[1;33mWarning\033[0m: Failed to load audio! Playing without audio...")
                no_audio_required = True

    # todo: dynamically correct speed
    # this is currently just a band-aid fix over a bigger wound
    if sys.platform == "darwin":
        speed_multiplier = 1.03
    else:
        speed_multiplier = 1.01
    wait_for = video_duration / speed_multiplier
    interval = (1 / (frame_rate * speed_multiplier))

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
    if not no_audio_required:
        pygame.mixer.music.play()
    current_interval = interval
    global lag

    try:
        for current_frame in range(total_frames):
            if child_error.qsize() > 0:
                os.kill(os.getpid(), signal.SIGINT)
            start_time = datetime.datetime.now()
            terminal_lines = os.get_terminal_size().lines
            if frames.qsize() < 1:
                if not no_audio_required:
                    pygame.mixer.music.pause()
                std_scr.clear()
                std_scr.addstr(0, 0, "Buffering...")
                std_scr.refresh()
                time.sleep(10)
                std_scr.clear()
                if not no_audio_required:
                    pygame.mixer.music.unpause()
            frame_list = frames.get(timeout=interval)
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
            except _curses.error:
                continue
            duration = (datetime.datetime.now() - start_time).total_seconds()
            if duration < current_interval:
                time.sleep(current_interval - duration)
            else:
                lag += 1
                current_interval = (duration - current_interval) / lag
            if current_interval < interval:
                current_interval = interval
        os.kill(os.getpid(), signal.SIGINT)
    finally:
        curses.echo()
        curses.nocbreak()
        curses.endwin()
        if child_error.qsize() > 0:
            return child_error.get()


if __name__ == '__main__':
    print("vidtty v1.0.0")
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
    if len(sys.argv) > 2:
        video_file = sys.argv[2]
    elif len(sys.argv) > 1:
        video_file = sys.argv[1]
    else:
        print("No video file specified. Please specify one. mp4 files works the best")
        video_file = None
        exit(1)
    if "-t" in sys.argv and len(sys.argv) > 3:

        tty = sys.argv[2] if sys.argv[1] == "-t" else sys.argv[3] if sys.argv[2] == "-t" else "/dev/stdout"
        video_file = sys.argv[3] if sys.argv[1] == "-t" else sys.argv[1]
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
    if sys.argv[1] in ["--no-audio", "-m"]:
        no_audio_required = True
        if len(sys.argv) > 2:
            video_file = sys.argv[2]
        else:
            print("No video file specified. Please specify one. mp4 files works the best")
            video_file = None
            exit(1)
    else:
        try:
            import pygame
        except ModuleNotFoundError:
            print("pygame not installed so there won't be any audio")
            no_audio_required = True
        else:
            no_audio_required = False
    if not os.path.exists(video_file):
        print(f"File \"{video_file}\" not found!")
        exit(1)
    if video_file.endswith(".vidtxt"):
        file_print_frames(video_file)
    else:
        video = cv2.VideoCapture(video_file)
        total_frames = int(video.get(cv2.CAP_PROP_FRAME_COUNT))
        frame_rate = video.get(cv2.CAP_PROP_FPS)
        frame_rate = 30 if not frame_rate else frame_rate
        video_duration = (total_frames // frame_rate) + (total_frames % frame_rate) / frame_rate
        global_interval = (1 / frame_rate)
        if sys.argv[1] in ["--dump", "-d"]:
            dump_frames(video_file)
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
