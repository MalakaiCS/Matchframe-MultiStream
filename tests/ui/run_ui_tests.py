#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# MatchFrame Multistream - real-OBS UI acceptance tests (Linux, Xvfb).
#
# Launches the real OBS Studio frontend with the plugin and the test-only driver plugin
# (tests/ui-driver), which clicks through the dock. This script provides local RTMP
# receivers, then verifies what actually arrived (decodable H.264/AAC, which programme
# audio and graphics were sent when), how long OBS took to exit, and that no stream key was
# written to any OBS log or configuration file.
#
# Linux: OBS installed under --obs-prefix (bin/obs), run under Xvfb with a private HOME.
# Windows: an extracted official OBS build under --obs-prefix (bin/64bit/obs64.exe), with the
# plugin and driver installed in %ProgramData%\obs-studio\plugins; OBS uses the real
# %APPDATA%\obs-studio, so run this only on a disposable machine (CI). Stream keys go to
# Windows Credential Manager there, and the restore scenario requires them to come back.
#
# Usage: run_ui_tests.py --obs-prefix DIR --work DIR --clips DIR [--make-clips] [scenario ...]

import argparse
import array
import json
import math
import os
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time

PORT_BASE = 19700
IS_WINDOWS = os.name == "nt"


def popen(args, **kwargs):
    """Starts a child in its own process group so it can be killed with everything it started."""
    if IS_WINDOWS:
        kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        kwargs["start_new_session"] = True
    return subprocess.Popen(args, **kwargs)


def kill_tree(proc):
    if proc.poll() is not None:
        return
    if IS_WINDOWS:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], capture_output=True)
    else:
        os.killpg(proc.pid, signal.SIGKILL)
    proc.wait()


class Receiver:
    def __init__(self, work, port, name):
        self.work, self.port, self.name = work, port, name
        self.key = f"uitest-{name}-{port}-K3y"
        self.segment = 0
        self.proc = None

    @property
    def url(self):
        return f"rtmp://127.0.0.1:{self.port}/live"

    def recording(self, segment=None):
        return os.path.join(self.work, f"ui-recv-{self.name}-{segment or self.segment}.flv")

    def start(self):
        self.segment += 1
        log = open(os.path.join(self.work, f"ui-recv-{self.name}.log"), "a")
        self.proc = popen(
            ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "warning", "-y", "-listen", "1",
             "-i", f"{self.url}/{self.key}", "-map", "0", "-c", "copy", "-f", "flv", self.recording()],
            stdout=log, stderr=log)
        if not wait_listening(self.port, 15.0):
            print(f"    [note] receiver {self.name} is not listening on port {self.port} "
                  f"(ffmpeg {'exited' if self.proc.poll() is not None else 'still starting'})", flush=True)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            if IS_WINDOWS:
                self.proc.terminate()  # no SIGINT for console children; the FLV data is already written
            else:
                self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def keep_listening(self):
        """Restarts the single-session ffmpeg listener whenever a publisher disconnects."""
        self._keep = True

        def loop():
            while self._keep:
                if self.proc and self.proc.poll() is not None and self._keep:
                    self.start()
                time.sleep(0.1)

        self._thread = threading.Thread(target=loop, daemon=True)
        self._thread.start()

    def stop_listening(self):
        self._keep = False
        if getattr(self, "_thread", None):
            self._thread.join()
        self.stop()

    def wait_exit(self, timeout=8):
        if not self.proc:
            return True
        try:
            self.proc.wait(timeout)
            return True
        except subprocess.TimeoutExpired:
            return False


def wait_listening(port, timeout=5.0):
    """Checks for a LISTEN socket without connecting (ffmpeg -listen 1 accepts one client)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if IS_WINDOWS:
            out = subprocess.run(["netstat", "-an", "-p", "TCP"], capture_output=True, text=True).stdout
            for line in out.splitlines():
                parts = line.split()
                if len(parts) >= 4 and parts[1].endswith(f":{port}") and parts[3] == "LISTENING":
                    return True
            time.sleep(0.1)
            continue
        for table in ("/proc/net/tcp", "/proc/net/tcp6"):
            if not os.path.exists(table):
                continue
            with open(table) as f:
                next(f)
                for line in f:
                    parts = line.split()
                    if int(parts[1].rsplit(":", 1)[1], 16) == port and parts[3] == "0A":
                        return True
        time.sleep(0.05)
    return False


def probe(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-count_frames", "-show_entries",
                          "stream=codec_type,codec_name,width,height,sample_rate,channels,nb_read_frames",
                          "-of", "json", path], capture_output=True, text=True)
    if out.returncode != 0:
        return None
    streams = json.loads(out.stdout).get("streams", [])
    info = {}
    for s in streams:
        info[s["codec_type"]] = s
    dec = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i", path, "-f", "null", "-"],
                         capture_output=True, text=True)
    info["decode_clean"] = dec.returncode == 0 and "rror" not in dec.stderr
    return info


def make_clips(directory):
    """The same two clips the libobs integration harness generates (tests/integration/obs-harness.cpp)."""
    os.makedirs(directory, exist_ok=True)
    a, b = os.path.join(directory, "clip-a.mp4"), os.path.join(directory, "clip-b.mp4")
    if not os.path.exists(a):
        # 60 s loop: moving test pattern; every 2 s a white flash and a 100 ms 1 kHz beep.
        subprocess.run(["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
                        "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30:duration=60",
                        "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000:duration=60",
                        "-filter_complex",
                        "[0:v]drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='lt(mod(t\\,2)\\,0.1)'[v];"
                        "[1:a]volume='if(lt(mod(t\\,2)\\,0.1)\\,1\\,0)':eval=frame,aformat=channel_layouts=stereo[a]",
                        "-map", "[v]", "-map", "[a]", "-c:v", "libx264", "-preset", "ultrafast", "-g", "30",
                        "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k", a], check=True)
    if not os.path.exists(b):
        subprocess.run(["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
                        "-f", "lavfi", "-i", "smptebars=size=320x180:rate=30:duration=60",
                        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=60",
                        "-af", "aformat=channel_layouts=stereo", "-c:v", "libx264", "-preset", "ultrafast",
                        "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k", b], check=True)


def corner_rgb(path, seconds):
    """Mean RGB of a 64x64 block near the top-left corner of the frame at the given time."""
    out = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-ss", f"{max(0.0, seconds):.2f}", "-i", path,
                          "-frames:v", "1", "-vf", "crop=64:64:40:30,scale=1:1:flags=area",
                          "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], capture_output=True).stdout
    return tuple(out[:3]) if len(out) >= 3 else None


def is_green(rgb):
    r, g, b = rgb
    return g > 170 and r < 90 and b < 90


def audio_windows(path, window=2.0):
    """Classifies consecutive windows as 'beep' (1 kHz), 'tone' (440 Hz) or 'silence'."""
    raw = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i", path, "-vn", "-ac", "1", "-ar", "8000",
                          "-f", "s16le", "-"], capture_output=True).stdout
    samples = array.array("h")
    samples.frombytes(raw[: len(raw) // 2 * 2])
    per = int(8000 * window)
    out = []
    for start in range(0, len(samples) - per + 1, per):
        block = samples[start:start + per]

        def goertzel(freq):
            coeff = 2 * math.cos(2 * math.pi * freq / 8000)
            s1 = s2 = 0.0
            total = 0.0
            for i, v in enumerate(block):
                s0 = v / 32768.0 + coeff * s1 - s2
                s2, s1 = s1, s0
                if (i + 1) % 800 == 0:
                    total += s1 * s1 + s2 * s2 - coeff * s1 * s2
                    s1 = s2 = 0.0
            return total

        rms = math.sqrt(sum((v / 32768.0) ** 2 for v in block) / len(block))
        e1000, e440 = goertzel(1000), goertzel(440)
        if rms < 0.003:
            label = "silence"
        elif e1000 > 5 * e440:
            label = "beep"
        elif e440 > 5 * e1000:
            label = "tone"
        else:
            label = "mixed"
        out.append(label)
    return out


class Run:
    def __init__(self, args):
        self.args = args
        self.work = os.path.abspath(args.work)
        os.makedirs(self.work, exist_ok=True)
        self.home = os.path.join(self.work, "obs-home")
        self.results = []
        self.failures = 0
        self.secrets = []
        self.crud_targets = []
        self.next_port = PORT_BASE

    def port(self):
        """Next port that can really be bound (Windows reserves port ranges per machine)."""
        while True:
            self.next_port += 1
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
                try:
                    probe.bind(("127.0.0.1", self.next_port))
                    return self.next_port
                except OSError:
                    print(f"    [note] port {self.next_port} cannot be bound here; skipping it", flush=True)

    def check(self, ok, what):
        print(f"    [{' ok ' if ok else 'FAIL'}] {what}", flush=True)
        self.results.append((ok, what))
        if not ok:
            self.failures += 1

    def config_dir(self):
        if IS_WINDOWS:
            return os.path.join(os.environ["APPDATA"], "obs-studio")
        return os.path.join(self.home, ".config", "obs-studio")

    def obs_env(self, config_path):
        env = dict(os.environ)
        env["MF_UITEST_CONFIG"] = config_path
        if not IS_WINDOWS:
            env.update({"HOME": self.home, "XDG_CONFIG_HOME": os.path.join(self.home, ".config"),
                        "LD_LIBRARY_PATH": os.path.join(self.args.obs_prefix, "lib"), "QT_QPA_PLATFORMTHEME": ""})
            env.setdefault("DISPLAY", ":99")
        return env

    def obs_command(self, *args):
        if IS_WINDOWS:
            bindir = os.path.join(self.args.obs_prefix, "bin", "64bit")
            return [os.path.join(bindir, "obs64.exe"), *args], bindir
        bindir = os.path.join(self.args.obs_prefix, "bin")
        return [os.path.join(bindir, "obs"), *args], bindir

    def seed(self, native_url, native_key):
        """First run creates OBS's configuration; then pin test-friendly profile settings."""
        for directory in (self.home, self.config_dir()):
            if os.path.exists(directory):
                shutil.rmtree(directory)
        os.makedirs(self.home)
        command, cwd = self.obs_command("--disable-updater", "--disable-shutdown-check")
        proc = popen(command, cwd=cwd, env=self.obs_env(os.path.join(self.work, "no-driver-config.json")),
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        user_ini = os.path.join(self.config_dir(), "user.ini")
        profile = os.path.join(self.config_dir(), "basic", "profiles", "Untitled")
        deadline = time.time() + 120
        while time.time() < deadline and proc.poll() is None:
            ready = os.path.exists(os.path.join(profile, "basic.ini")) and os.path.exists(user_ini)
            if ready and "FirstRun=true" in open(user_ini, errors="replace").read():
                break
            time.sleep(0.5)
        time.sleep(3)
        print(f"    [note] first OBS run (creates its configuration) exited early: {proc.poll() is not None}")
        kill_tree(proc)
        with open(user_ini, "a") as f:
            f.write("\n[Basic]\nConfigOnNewProfile=false\n")
        rec = os.path.join(self.work, "obs-recordings")
        os.makedirs(rec, exist_ok=True)
        with open(os.path.join(profile, "basic.ini"), "a") as f:
            f.write("\n[Video]\nBaseCX=1280\nBaseCY=720\nOutputCX=1280\nOutputCY=720\nFPSType=0\nFPSCommon=30\n"
                    "[Audio]\nSampleRate=48000\nChannelSetup=Stereo\n"
                    f"[Output]\nMode=Simple\n[SimpleOutput]\nFilePath={rec}\nRecFormat2=mkv\nRecRB=true\nRecRBTime=10\n"
                    "VBitrate=2500\nABitrate=128\n")
        with open(os.path.join(profile, "service.json"), "w") as f:
            json.dump({"type": "rtmp_custom", "settings": {"server": native_url, "key": native_key}}, f)

    def launch(self, scenario, extra, timeout=300):
        results = os.path.join(self.work, f"driver-{scenario}.jsonl")
        if os.path.exists(results):
            os.remove(results)
        config = dict(extra)
        config.update({"scenario": scenario, "results": results, "clipA": self.args.clips + "/clip-a.mp4",
                       "clipB": self.args.clips + "/clip-b.mp4"})
        config_path = os.path.join(self.work, f"driver-{scenario}.json")
        with open(config_path, "w") as f:
            json.dump(config, f)
        started = time.time()
        command, cwd = self.obs_command("--disable-updater", "--disable-shutdown-check", "--collection", "Untitled")
        proc = popen(command, cwd=cwd, env=self.obs_env(config_path),
                     stdout=open(os.path.join(self.work, f"obs-{scenario}.stdout"), "w"), stderr=subprocess.STDOUT)
        try:
            proc.wait(timeout)
        except subprocess.TimeoutExpired:
            kill_tree(proc)
            self.check(False, f"{scenario}: OBS did not exit within {timeout} s")
        steps = []
        if os.path.exists(results):
            with open(results) as f:
                steps = [json.loads(line) for line in f if line.strip()]
        for step in steps:
            if step["step"].startswith("mark:") or step["step"] in ("scenario",):
                continue
            self.check(step["ok"], f"{scenario}: {step['step']}" + (f" ({step['detail']})" if step["detail"] else ""))
        self.check(any(s["step"] == "finished" for s in steps), f"{scenario}: driver completed every step")
        marks = {s["step"][5:]: s["t"] / 1000.0 for s in steps if s["step"].startswith("mark:")}
        return steps, marks, time.time() - started, proc.returncode

    def verify(self, receiver, label, min_seconds, segment=None):
        info = probe(receiver.recording(segment))
        ok = info is not None
        video = (info or {}).get("video", {})
        audio = (info or {}).get("audio", {})
        frames = int(video.get("nb_read_frames", 0) or 0)
        print(f"    [note] {label}: {video.get('codec_name')} {video.get('width')}x{video.get('height')} "
              f"{frames} frames | {audio.get('codec_name')} {audio.get('sample_rate')} Hz "
              f"{audio.get('channels')}ch | decode {'clean' if info and info['decode_clean'] else 'ERRORS'}")
        self.check(ok and video.get("codec_name") == "h264" and audio.get("codec_name") == "aac"
                   and info["decode_clean"] and frames >= min_seconds * 30,
                   f"{label}: decodable H.264 + AAC, >= {min_seconds} s")

    def scan_for_secrets(self, extra_files=()):
        hits = []
        paths = []
        for root, _, files in os.walk(self.config_dir()):
            paths += [os.path.join(root, f) for f in files]
        paths += list(extra_files)
        for path in paths:
            if path.endswith("service.json"):
                continue  # OBS's own native-streaming service settings, not written by the plugin
            try:
                data = open(path, "rb").read()
            except OSError:
                continue
            for secret in self.secrets:
                if secret.encode() in data:
                    hits.append(f"{os.path.relpath(path, self.work)} contains a test key")
        self.check(not hits, "no test stream key appears in any OBS log, plugin config, profile or export file"
                   + (": " + "; ".join(hits[:5]) if hits else f" ({len(paths)} files scanned)"))

    def plugin_log_lines(self):
        logs = os.path.join(self.config_dir(), "logs")
        lines = []
        for name in sorted(os.listdir(logs)):
            with open(os.path.join(logs, name), errors="replace") as f:
                lines += [l.rstrip() for l in f if "matchframe-multistream" in l or "mf-ui-driver" in l]
        return lines


def scenario_crud_restore(run):
    print("\n[ RUN  ] crud + restore (acceptance 1, 10)")
    a, b = Receiver(run.work, run.port(), "crud-a"), Receiver(run.work, run.port(), "crud-b")
    run.secrets += [a.key, b.key]
    a.start()
    export_path = os.path.join(run.work, "export.json")
    steps, _, _, _ = run.launch("crud", {"exportPath": export_path,
                                         "destinations": [{"url": a.url, "key": a.key}, {"url": b.url, "key": b.key}]})
    a.wait_exit()
    run.verify(a, "crud receiver", 4)
    exported = json.load(open(export_path)) if os.path.exists(export_path) else {}
    run.check(exported.get("schema") == "matchframe-multistream.export" and len(exported.get("destinations", [])) == 2,
              "export file written with 2 destinations")
    if IS_WINDOWS:
        finished = [s["detail"] for s in steps if s["step"] == "finished"]
        run.check(finished == ["session-only consents: 0"],
                  f"keys saved to Windows Credential Manager without a session-only fallback ({finished})")
        targets = credential_targets()
        run.check(len(targets) >= 2, f"Credential Manager holds the plugin's key entries ({len(targets)} found)")
        run.crud_targets = targets
        b.start()
    steps, _, _, _ = run.launch("restore", {"expectStoredKeys": IS_WINDOWS})
    status = [s["detail"] for s in steps if s["step"] == "key status after restart"]
    store = "Windows Credential Manager" if IS_WINDOWS else "none on Linux, so keys were session-only"
    print(f"    [note] key status after restart (secure store: {store}): {status}")
    if IS_WINDOWS:
        b.wait_exit()
        run.verify(b, "restore receiver (key read back from Credential Manager)", 4)
    run.scan_for_secrets([export_path])
    b.stop()


def credential_targets():
    """Windows only: the plugin's generic credentials for the current user (names only, never values)."""
    out = subprocess.run(["cmdkey", "/list"], capture_output=True, text=True).stdout
    return [line.split("=", 1)[1].strip() for line in out.splitlines()
            if "MatchFrameMultistream/v1/" in line and "=" in line]


def scenario_eight(run):
    print("\n[ RUN  ] eight destinations from the dock (acceptance 2, 3, 4)")
    receivers = [Receiver(run.work, run.port(), f"eight-{i}") for i in range(8)]
    for r in receivers:
        r.start()
        run.secrets.append(r.key)
    run.launch("eight", {"destinations": [{"url": r.url, "key": r.key} for r in receivers]})
    if IS_WINDOWS and run.crud_targets:
        remaining = credential_targets()
        run.check(not set(run.crud_targets) & set(remaining) and len(remaining) == 8,
                  f"deleting destinations removed their keys from Credential Manager ({len(remaining)} entries left)")
    for r in receivers:
        r.wait_exit()
    run.verify(receivers[0], "receiver 1 (stopped early)", 6)
    for i, r in enumerate(receivers[1:], start=2):
        run.verify(r, f"receiver {i}", 14)
    shared = [l for l in run.plugin_log_lines() if "shared encoders created" in l]
    print(f"    [note] {shared[-1] if shared else 'no encoder log line'}")


def scenario_program(run):
    print("\n[ RUN  ] programme feed: Studio Mode, transitions, graphics, native outputs, muting (acceptance 8, 9)")
    r = Receiver(run.work, run.port(), "program")
    run.secrets.append(r.key)
    r.start()
    steps, marks, _, _ = run.launch("program", {"destinations": [{"url": r.url, "key": r.key}]}, timeout=360)
    r.wait_exit()
    run.verify(r, "programme receiver", 30)
    labels = audio_windows(r.recording())
    print(f"    [note] 2 s audio windows: {' '.join(labels)}")
    print(f"    [note] driver marks (s): { {k: round(v, 1) for k, v in marks.items()} }")
    if "sending" in marks and "program_b" in marks:
        # The recording starts at the first keyframe after the connection (<= 2 s after "sending").
        offset = marks["sending"]
        def window(t):
            return int((t - offset) // 2)
        before_b = labels[: max(0, window(marks["program_b"]) - 1)]
        preview_period = labels[window(marks["preview_b"]) + 1: window(marks["program_b"]) - 1]
        run.check(len(before_b) >= 4 and all(l == "beep" for l in before_b),
                  "programme audio stays on Cam A until the transition, including while Cam B was only in Preview")
        run.check(preview_period and all(l == "beep" for l in preview_period),
                  "Studio Mode preview change alone did not alter the output")
        after_b = labels[window(marks["program_b"]) + 2: window(marks["muted"]) - 1]
        run.check(after_b and all(l == "tone" for l in after_b),
                  "after the transition the output carries Cam B (440 Hz) through native stream/record/replay tests")
        muted = labels[window(marks["muted"]) + 1: window(marks["program_a_again"])]
        run.check(muted and all(l == "silence" for l in muted), "muting the on-programme source silences the output")
        # The cut fades for 300 ms and OBS buffers audio, so on a slow machine the first window after
        # the mark can still hold the fade; allow exactly one such "mixed" window, then require Cam A.
        back = labels[window(marks["program_a_again"]) + 1: window(marks["stopped"]) - 1]
        if back and back[0] == "mixed":
            back = back[1:]
        run.check(back and all(l == "beep" for l in back), "cutting back to Cam A returns its audio")
    if all(k in marks for k in ("sending", "graphic_in_preview", "program_b", "program_a_again")):
        # Sample the top-left corner well inside each period; the recording lags "sending" by <= 2 s.
        offset = marks["sending"]
        preview = corner_rgb(r.recording(), marks["graphic_in_preview"] + 3 - offset)
        program = corner_rgb(r.recording(), marks["program_b"] + 4 - offset)
        after = corner_rgb(r.recording(), marks["program_a_again"] + 4 - offset)
        print(f"    [note] top-left RGB: graphic in Preview only {preview}, after transition {program}, "
              f"after cut to Cam A {after}")
        run.check(preview is not None and not is_green(preview),
                  "a graphic added to the Preview scene does not reach the output")
        run.check(program is not None and is_green(program),
                  "after the transition the graphic on Program appears in the output")
        run.check(after is not None and not is_green(after), "graphic gone after cutting to a scene without it")
    recordings = [f for f in os.listdir(os.path.join(run.work, "obs-recordings"))]
    print(f"    [note] native OBS recordings/replays: {recordings}")
    run.check(any(f.endswith(".mkv") for f in recordings), "native recording and replay files were written")


def scenario_profile_exit(run):
    print("\n[ RUN  ] profile change, exit while live, no auto-start (acceptance 10)")
    r = Receiver(run.work, run.port(), "profile")
    run.secrets.append(r.key)
    r.start()
    r.keep_listening()
    steps, marks, elapsed, code = run.launch("profile_exit", {"destinations": [{"url": r.url, "key": r.key}]})
    sessions = r.segment
    r.stop_listening()
    exit_request = marks.get("exit_requested")
    print(f"    [note] OBS ran {elapsed:.1f} s; exit code {code}")
    run.check(code == 0, "OBS exited cleanly while a plugin output was live")
    lines = run.plugin_log_lines()
    shutdown = [l for l in lines if "plugin outputs shut down in" in l]
    print(f"    [note] {shutdown[-1] if shutdown else 'no shutdown log line'}")
    run.check(bool(shutdown), "plugin stopped its outputs during OBS exit (bounded teardown)")
    # The first segment ended when the profile switched; the second session started later.
    run.verify(r, "profile receiver (first session, ended by the profile switch)", 2, segment=1)
    run.verify(r, "profile receiver (second session, ended by OBS exit)", 2, segment=2)
    print(f"    [note] receiver sessions during the run: {sessions - 1} (listener restarts after each disconnect)")
    r.start()
    run.launch("no_autostart", {})
    still_listening = r.proc.poll() is None
    run.check(still_listening, "receiver saw no connection after relaunch (nothing auto-started)")
    r.stop()


SCENARIOS = {"crud": scenario_crud_restore, "eight": scenario_eight, "program": scenario_program,
             "profile_exit": scenario_profile_exit}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--obs-prefix", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--clips", required=True, help="directory with clip-a.mp4 and clip-b.mp4")
    parser.add_argument("--make-clips", action="store_true", help="generate the clips if they are missing")
    parser.add_argument("scenarios", nargs="*")
    args = parser.parse_args()
    if args.make_clips:
        make_clips(args.clips)
    run = Run(args)
    native = Receiver(run.work, run.port(), "native")
    run.seed(native.url, native.key)
    native.start()
    for name in args.scenarios or list(SCENARIOS):
        if name == "program" and native.proc.poll() is not None:
            native.start()
        SCENARIOS[name](run)
    native.stop()
    print(f"\n{len(run.results)} checks, {run.failures} failed")
    with open(os.path.join(run.work, "ui-report.txt"), "w") as f:
        for ok, what in run.results:
            f.write(f"{'PASS' if ok else 'FAIL'} {what}\n")
    return 1 if run.failures else 0


if __name__ == "__main__":
    sys.exit(main())
