#!/usr/bin/env python3
"""DVD menu tests: drive the app through the synthetic test DVD.

    tools/test-dvd-menus.py [--app build/app/playser] [--keep DIR]

Builds the fixture (tools/make-test-dvd.sh) if it's missing, runs the app
on it with scripted input (muted), and checks screenshots and the
navigation log. Exits non-zero if a check fails. Needs a desktop session
(mpv opens a window) and Pillow.
"""
import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "build/fixtures/discnav-test/discnav-test.iso"

# test DVD geometry (720x480 storage, 4:3), from tools/make-test-dvd.sh
MENU_BG = (0x10, 0x18, 0x30)
VMGM_BUTTONS = [(200, 150, 520, 200), (200, 230, 520, 280), (200, 310, 520, 360)]
VTS_BUTTONS = [(100, 360, 330, 410), (390, 360, 620, 410)]
SUB_BG = (0x20, 0x30, 0x10)

failures = []


def check(name, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  ({detail})" if detail else ""))
    if not ok:
        failures.append(name)


def run(app, script, shots):
    shots.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, QT_FORCE_STDERR_LOGGING="1", QT_LOGGING_RULES="playser.discnav.debug=true")
    p = subprocess.run([app, "--mpv-option", "mute=yes", "--play-menu", str(FIXTURE),
                        "--shot-dir", str(shots), "--script", script],
                       env=env, capture_output=True, text=True, timeout=120)
    return [l.split("playser.discnav: ", 1)[1] for l in p.stderr.splitlines() if "playser.discnav: " in l]


class Shot:
    """A window screenshot, addressed in the DVD's storage coordinates."""

    def __init__(self, path):
        self.im = Image.open(path).convert("RGB")
        w, h = self.im.size
        self.vw, self.vh = h * 4 / 3, h  # 4:3 pillarboxed in the window
        self.x0 = (w - self.vw) / 2

    def at(self, x, y):
        return self.im.getpixel((int(self.x0 + x / 720 * self.vw), int(y / 480 * self.vh)))


def near(c, ref, tol=40):
    return all(abs(a - b) <= tol for a, b in zip(c, ref))


def yellow(c):
    return c[0] > 200 and c[1] > 200 and c[2] < 90


def outlined(shot, button):
    """The button's highlight outline (6 px, yellow) is drawn on its edge."""
    x0, y0, x1, y1 = button
    return yellow(shot.at(x0 + 2, (y0 + y1) // 2)) and yellow(shot.at((x0 + x1) // 2, y0 + 2))


def ui_shown(path):
    """The UI layer has something drawn (the home screen), not just video."""
    im = Image.open(path).convert("RGBA")
    return im.getpixel((im.width // 2, im.height // 2))[3] > 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", default=str(ROOT / "build/app/playser"))
    ap.add_argument("--keep", help="keep screenshots in this directory")
    opt = ap.parse_args()
    if not FIXTURE.exists():
        subprocess.run([str(ROOT / "tools/make-test-dvd.sh")], cwd=ROOT, check=True)
    out = pathlib.Path(opt.keep) if opt.keep else pathlib.Path(tempfile.mkdtemp(prefix="dvd-menus-"))

    # 1. still menu, highlight moves, title 2 with its forced subtitle, and
    #    back to the menu when it ends (its post command)
    log = run(opt.app, "4,shot,down,1,shot,enter,4,shot,13,shot,quit", out / "a")
    s = [Shot(out / f"a/shot-0{i}.png") for i in range(1, 5)]
    check("menu shown", near(s[0].at(100, 450), MENU_BG), str(s[0].at(100, 450)))
    check("first button highlighted", outlined(s[0], VMGM_BUTTONS[0]) and not outlined(s[0], VMGM_BUTTONS[1]))
    check("down moves the highlight", outlined(s[1], VMGM_BUTTONS[1]) and not outlined(s[1], VMGM_BUTTONS[0]))
    check("title 2 plays", "title" in log and not near(s[2].at(100, 100), MENU_BG), str(s[2].at(100, 100)))
    check("subtitle in the disc's colours", near(s[2].at(270, 210), (255, 128, 0)) and near(s[2].at(360, 240), (0, 255, 255)),
          f"{s[2].at(270, 210)} {s[2].at(360, 240)}")
    check("title end returns to the menu", near(s[3].at(100, 450), MENU_BG) and outlined(s[3], VMGM_BUTTONS[0]),
          str(s[3].at(100, 450)))

    # 2. motion menu, its buttons side by side, title 1 from it, chapter
    #    skip, seek, the disc menu from the title, its submenu (the disc has
    #    no "go up": B goes to the main menu), B at the main menu goes home
    log = run(opt.app, "4,down,down,enter,3,shot,right,1,shot,enter,3,pgdn,2,left,2,menu,3,shot,"
                       "enter,3,shot,back,3,shot,back,3,shot,quit", out / "b")
    s = [Shot(out / f"b/shot-0{i}.png") for i in range(1, 6)]
    check("motion menu, first button", outlined(s[0], VTS_BUTTONS[0]) and not outlined(s[0], VTS_BUTTONS[1]))
    check("right moves the highlight", outlined(s[1], VTS_BUTTONS[1]))
    check("chapter skip", "user: next chapter" in log)
    check("seek in a title", any(l.startswith("user: seek to") and "refused" not in l for l in log))
    check("disc menu from a title", "user: root menu" in log and outlined(s[2], VTS_BUTTONS[0]))
    check("submenu", near(s[3].at(100, 450), SUB_BG), str(s[3].at(100, 450)))
    check("B in a submenu goes to the main menu", "go up: not on this disc, the main menu instead" in log
          and outlined(s[4], VTS_BUTTONS[0]))
    check("B at the main menu goes home", "user: go up (refused)" in log and ui_shown(out / "b/shot-06-ui.png"))

    # 3. the track picker lists the disc's streams and chooses through it
    log = run(opt.app, "4,down,enter,2,info,1,right,down,enter,1,quit", out / "c")
    check("subtitles chosen in the picker", "user: subtitle stream 0" in log)

    print(f"\n{len(failures)} failed: {', '.join(failures)}" if failures else "\nall passed", f"(screenshots: {out})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
