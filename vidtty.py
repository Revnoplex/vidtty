#!/usr/bin/env python3
import json
import shutil
import signal
import struct
import subprocess
import traceback
from io import BytesIO
import time
from multiprocessing import Manager, Process, Queue, Value
import queue as queue_mod
import sys
import ctypes
import datetime
import threading
from types import TracebackType
from typing import Union
from PIL import Image
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


def check_for_errors(command: subprocess.Popen):
    if command.returncode:
        return command.stderr.read()

    def enqueue_output(out, pq):
        pq.put(out.read())

    q = queue_mod.Queue()
    t = threading.Thread(target=enqueue_output, args=(command.stderr, q))
    t.daemon = True  # thread dies with the program
    t.start()

    try:
        line = q.get(timeout=.1)
    except queue_mod.Empty:
        return
    else:
        return line


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
        print("\x1b[1;31mFatal\x1b[0m: Cannot dump frames as the fps value cannot be stored as a 64 bit double")
        return
    # byte numbers:      0   1   2   3   4   5   6   7                            8 to 11
    initial_header = b'\x56\x49\x44\x54\x58\x54\x00\x00' + terminal_columns.to_bytes(4, "big", signed=False) + \
                     terminal_lines.to_bytes(4, "big", signed=False) + struct.pack("d", fps)
    #                                          12 to 15                 16 to 23
    #                           24 to 63
    mem_file = initial_header + b'\x00' * (64 - len(initial_header))
    raw_video = subprocess.Popen(["ffmpeg", "-nostdin", "-i", video_filename, "-loglevel", "error", "-s",
                                  f"{terminal_columns}x{terminal_lines}", "-c:v", "bmp", "-f", "rawvideo", "-an",
                                  "pipe:1"],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    raw_video_errors = check_for_errors(raw_video)
    if raw_video_errors:
        print("\x1b[1;31mFatal\x1b[0m: Failed to read video:")
        print(raw_video_errors.decode("utf-8"))
        return
    if no_audio_required:
        # 24 to 32
        mem_file = mem_file[:24] + b'\x00' * 8 + mem_file[32:]
    else:
        print("Extracting audio from video file...")
        ffmpeg_options = ["ffmpeg", "-nostdin"] + (["-reconnect", "1", "-reconnect_streamed", "1",
                                                    "-reconnect_delay_max", "5"] if url else []) + \
                         ["-i", video_file, "-loglevel", "error", "-f", "mp3", "pipe:1"]
        audio = subprocess.Popen(ffmpeg_options, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        audio_errors = check_for_errors(audio)
        if audio_errors:
            print("\x1b[1;33mWarning\x1b[0m: Extracting audio failed:")
            print(audio_errors.decode("utf-8"))
            print("Continuing without audio...")
            mem_file = mem_file[:24] + b'\x00' * 8 + mem_file[32:]
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
        # if not video.isOpened():
        #     print("\x1b[1;31mFatal\x1b[0m: Failed to open video", file=sys.stderr)
        #     return
        # need new fail checker
        average_interval = 1.0
        if len(avg_interval_list) > 0:
            average_interval = sum(avg_interval_list) / len(avg_interval_list)
        average_fps = round(1 / average_interval, 1)
        time_left = average_interval * (total_frames - current_frame)
        progress_text = f"\x1b[7m\rDumping Frame: {current_frame}/{total_frames} " \
                        f" Rate: {average_fps}/s ETA:" \
                        f" {str(datetime.timedelta(seconds=time_left)).split('.')[0]}"
        percentage = f"[ {round(current_frame / total_frames*100)}% ]"
        progress_text = \
            progress_text + " "*(os.get_terminal_size().columns-((len(progress_text)-5)+len(percentage))) + percentage
        # print("\r" + repr(progress_text), end="")
        progress_pos = round(current_frame / total_frames*os.get_terminal_size().columns) + 5
        # print(progress_pos)
        if progress_pos > 1:
            progress_text = progress_text[:progress_pos+1] + "\x1b[0m" + progress_text[progress_pos+1:]
        else:
            progress_text = progress_text[:2] + "\x1b[0m" + progress_text[:2]
        # print("\r" + progress_text[progress_pos+1:], end="")
        print(progress_text, end="")
        raw_video.stdout.read(2)
        current_size = int.from_bytes(raw_video.stdout.read(4), "little", signed=False)
        if current_size >= 6:
            raw_video_bin = b'BM' + current_size.to_bytes(4, "little", signed=False) + \
                            raw_video.stdout.read(current_size - 6)
        else:
            break
        if current_size < 1:
            break
        frame = Image.open(BytesIO(raw_video_bin))
        img_data = frame.getdata()
        ascii_gradients = [' ', '.', "'", '`', '^', '"', ',', ':', ';', 'I', 'l', '!', 'i', '>', '<', '~', '+',
                           '_', '-', '?', ']', '[', '}', '{', '1', ')', '(', '|', '\\', '/', 't', 'f', 'j', 'r',
                           'x', 'n', 'u', 'v', 'c', 'z', 'X', 'Y', 'U', 'J', 'C', 'L', 'Q', '0', 'O', 'Z', 'm',
                           'w', 'q', 'p', 'd', 'b', 'k', 'h', 'a', 'o', '*', '#', 'M', 'W', '&', '8', '%', 'B',
                           '@', '$']
        frame_width = frame.width
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
        current_frame = 0
        avg_interval_list = []
        terminal_lines, terminal_columns = (lambda px: (px.lines, px.columns))(os.get_terminal_size())
        raw_video = subprocess.Popen(["ffmpeg", "-nostdin", "-i", video_filename, "-loglevel", "error", "-s",
                                      f"{terminal_columns}x{terminal_lines}", "-c:v", "bmp", "-f", "rawvideo", "-an",
                                      "pipe:1"],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        raw_video_errors = check_for_errors(raw_video)
        if raw_video_errors:
            print("\x1b[1;31mFatal\x1b[0m: Failed to read video:")
            print(raw_video_errors.decode("utf-8"))
            return
        while True:
            start_time = datetime.datetime.now()
            average_interval = 1.0
            if len(avg_interval_list) > 0:
                average_interval = sum(avg_interval_list)/len(avg_interval_list)
            dumping_interval.value = average_interval
            dumped_frames.value = current_frame
            raw_video.stdout.read(2)
            current_size = int.from_bytes(raw_video.stdout.read(4), "little", signed=False)
            if current_size >= 6:
                raw_video_bin = b'BM' + current_size.to_bytes(4, "little", signed=False) + \
                                raw_video.stdout.read(current_size - 6)
            else:
                break
            if current_size < 1:
                break
            frame = Image.open(BytesIO(raw_video_bin))

            img_data = frame.getdata()
            ascii_gradients = [' ', '.', "'", '`', '^', '"', ',', ':', ';', 'I', 'l', '!', 'i', '>', '<', '~', '+',
                               '_', '-', '?', ']', '[', '}', '{', '1', ')', '(', '|', '\\', '/', 't', 'f', 'j', 'r',
                               'x', 'n', 'u', 'v', 'c', 'z', 'X', 'Y', 'U', 'J', 'C', 'L', 'Q', '0', 'O', 'Z', 'm',
                               'w', 'q', 'p', 'd', 'b', 'k', 'h', 'a', 'o', '*', '#', 'M', 'W', '&', '8', '%', 'B',
                               '@', '$']
            frame_width = frame.width
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
            blank_sound = subprocess.Popen(
                ["aplay", "--quiet"] if shutil.which("aplay") else ["play", "-q", "-V1", "-t",
                                                                    "wav", "-"],
                stdin=subprocess.PIPE)
            blank_sound.communicate(input=b'RIFF%\x00\x00\x00WAVEfmt \x10\x00\x00\x00\x01\x00\x01\x00D\xac\x00\x00\x88X'
                                          b'\x01\x00\x02\x00\x10\x00datat\x00\x00\x00\x00')
            audio = subprocess.Popen(["ffmpeg", "-nostdin", "-i", "-", "-loglevel", "error", "-f", "wav", "pipe:1"],
                                     stdin=vidtxt_file, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            audio_errors = check_for_errors(audio)
            if audio_errors:
                print("\x1b[1;31mFatal\x1b[0m: Failed to read audio:")
                print(audio_errors.decode("utf-8"))
            audio_cmd = subprocess.Popen(["aplay", "--quiet"] if shutil.which("aplay") else ["play", "-q", "-V1", "-t",
                                                                                             "wav", "-"],
                                         stdin=audio.stdout, stderr=subprocess.PIPE)
            audio_cmd_errors = check_for_errors(audio_cmd)
            if audio_cmd_errors:
                print("\x1b[1;31mFatal\x1b[0m: Failed to play audio:")
                print(audio_cmd_errors.decode("utf-8"))
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
                        debug_text = f"[Frame: ({calculated_frames},{frame_number},{frames_behind}), " \
                                     f"{str(time_elapsed).split('.')[0]}]"
                        end_text = f"[{str(datetime.timedelta(seconds=vid_duration)).split('.')[0]}, " \
                                   f"{f_total_frames} frames, " \
                                   f"{round(calculated_frames / f_total_frames * 100, 1)}%] "
                        if len(debug_text) < current_terminal_columns - 1:
                            debug_text = debug_text + " " * (
                                          current_terminal_columns - (len(debug_text) + len(end_text))) + end_text
                        progress = round(calculated_frames / f_total_frames * current_terminal_columns)
                        for idx, char in enumerate(debug_text):
                            if idx < current_terminal_columns - 1:
                                f_format = curses.A_STANDOUT if idx < progress else curses.A_NORMAL
                                try:
                                    std_scr.addch(current_terminal_lines - 1, idx, char, f_format)
                                except _curses.error:
                                    pass
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
    if not no_audio_required:
        print("Extracting audio from video file...")
        ffmpeg_options = ["ffmpeg", "-nostdin"] + (["-reconnect", "1", "-reconnect_streamed", "1",
                                                    "-reconnect_delay_max", "5"] if url else []) + \
                         ["-i", video_file, "-loglevel", "error", "-f", "wav", "pipe:1"]
        audio = subprocess.Popen(ffmpeg_options, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        audio_errors = check_for_errors(audio)
        if audio_errors:
            print("\x1b[1;33mWarning\x1b[0m: Extracting audio failed:")
            print(audio_errors.decode("utf-8"))
            print("Continuing without audio...")
            no_audio_required = True
    else:
        audio = None
    wait_for = video_duration
    interval = 1 / frame_rate

    while True:
        average_fps = round(1 / dumping_interval.value, 1)
        time_left = dumping_interval.value * (total_frames-dumped_frames.value)
        if not time_left > wait_for:
            break
        if child_error.qsize() > 0:
            return child_error.get()
        print(f"\rRendering Frame: {dumped_frames.value}/{total_frames} "
              f"Rate: {average_fps}/s Playback ETA:"
              f" {str(datetime.timedelta(seconds=(time_left-video_duration))).split('.')[0]}", end="")

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
        audio_cmd_errors = check_for_errors(audio_cmd)
        if audio_cmd_errors:
            print("\x1b[1;31mFatal\x1b[0m: Failed to read audio:")
            print(audio_cmd_errors.decode("utf-8"))
    current_interval = interval
    displayed_since = datetime.datetime.now()
    global lag
    race_condition_error = False

    try:
        for current_frame in range(total_frames):
            if child_error.qsize() > 0:
                os.kill(os.getpid(), signal.SIGINT)
            start_time = datetime.datetime.now()
            terminal_lines = os.get_terminal_size().lines
            terminal_columns = os.get_terminal_size().columns
            if frames.qsize() < 1:
                if not no_audio_required:
                    audio_cmd.kill()
                race_condition_error = True
                break
                # std_scr.clear()
                # std_scr.addstr(0, 0, "Buffering...")
                # std_scr.refresh()
                # time.sleep(10)
                # std_scr.clear()
                # if not no_audio_required:
                #     audio_cmd.send_signal(18)
                # displayed_since + datetime.timedelta(seconds=10)
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
                    debug_text = f"[Frame: ({calculated_frames},{frame_number},{frames_behind}), " \
                                 f"{str(time_elapsed).split('.')[0]}]"
                    end_text = f"[{str(datetime.timedelta(seconds=video_duration)).split('.')[0]}, " \
                               f"{total_frames} frames, " \
                               f"{round(calculated_frames/total_frames*100, 1)}%] "
                    if len(debug_text) < terminal_columns - 1:
                        debug_text = debug_text + " "*(terminal_columns-(len(debug_text)+len(end_text))) + end_text
                    progress = round(calculated_frames/total_frames*terminal_columns)
                    for idx, char in enumerate(debug_text):
                        if idx < terminal_columns - 1:
                            f_format = curses.A_STANDOUT if idx < progress else curses.A_NORMAL
                            try:
                                std_scr.addch(terminal_lines - 1, idx, char, f_format)
                            except _curses.error:
                                pass
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
        if race_condition_error:
            exit(2)


if __name__ == '__main__':
    print("vidtty v1.1.0")
    if sys.platform not in ["linux", "darwin"]:
        print("\x1b[1;33mWarning\x1b[0m: This version of vidtty has only been tested to on Unix based OSes such as"
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
        print(f"\x1b[1;31mFatal\x1b[0m: curses module not found. Please make sure you have the package installed.")
        exit(1)
    video_file = sys.argv[-1]
    options = sys.argv[1:-1]
    if set(options + [video_file]).intersection({"--help", "-h"}) or len(sys.argv) < 2:
        print("\x1b[1mHelp Menu\x1b[0m")
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
    if not shutil.which("ffmpeg"):
        print(f"\x1b[1;31mFatal\x1b[0m: ffmpeg executable not found. Please make sure ffmpeg is installed and make sure"
              f" the executable is in your PATH.", file=sys.stderr)
        print(f"To use without audio and bypass these errors, pass the -m or --no-audio argument")
        exit(1)
    if set(options).intersection({"--no-audio", "-m"}):
        no_audio_required = True
        if len(sys.argv) > 2:
            pass
        else:
            print("No video file specified. Please specify one. mp4 files works the best")
            video_file = None
            exit(1)
    elif not (shutil.which("aplay") or shutil.which("play")):
        print(f"\x1b[1;31mFatal\x1b[0m: aplay or play executable not found. "
              f"Please make sure alsa-utils or sox is installed and make "
              f"sure the executable is in your PATH.")
        print(f"To use without audio and bypass these errors, pass the -m or --no-audio argument")
        no_audio_required = True
        exit(1)
    else:
        no_audio_required = False
    url = False
    if video_file.startswith("http://") or video_file.startswith("https://"):
        url = True
    if not url and not os.path.exists(video_file):
        print(f"File \"{video_file}\" not found!")
        exit(1)
    if not url:
        with open(video_file, "rb") as vidtxt_check:
            first_8 = vidtxt_check.read(8)
    if (not url) and (video_file.endswith(".vidtxt") or first_8 == b'VIDTXT\x00\x00'):
        file_print_frames(video_file)
    else:
        ffprobe = subprocess.Popen(["ffprobe", "-hide_banner", "-show_streams", "-of", "json", video_file],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        ffprobe.wait()
        if ffprobe.returncode:
            print("\x1b[1;31mFatal\x1b[0m: Failed to extract video metadata:")
            print(ffprobe.stderr.read().decode("utf-8"))
            exit(1)
        try:
            file_metadata = json.load(ffprobe.stdout).get('streams')[0]
            total_frames = int(file_metadata.get("nb_frames"))
            fps_fraction = file_metadata.get("r_frame_rate").split("/")
            frame_rate = float(int(fps_fraction[0])/int(fps_fraction[1]))
        except (ValueError, TypeError, IndexError, json.JSONDecodeError) as err:
            err: BaseException
            print("\x1b[1;31mFatal\x1b[0m: Failed to extract video metadata:\nUnexpected or missing metadata. "
                  "Is this file a video?")
            if debug_mode:
                print(str(err))
            exit(1)
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
                                                        shared_child_error,))
                p1.exception = exception_handler
                p1.start()
                child_error_state = print_frames(queue, shared_dumped_frames, shared_dumping_interval,
                                                 shared_child_error)
                if child_error_state:
                    exception_handler(*child_error_state)
            finally:
                p1.terminate()
