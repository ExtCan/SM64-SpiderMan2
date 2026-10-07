#!/usr/bin/env python3
"""Checks an end-to-end run (tests/run_e2e.sh): the host's diagnostics and the
mod's log must show Mario inside the frame and walking on what was rendered.

    tests/check_e2e.py <work-dir>/game
"""
import os
import pathlib
import re
import sys


def main() -> int:
    game = pathlib.Path(sys.argv[1])
    host = (game / "host.out").read_text(errors="replace")
    log = (game / "sm2mario" / "sm2mario.log").read_text(errors="replace")
    failures = 0

    def check(ok: bool, what: str) -> None:
        nonlocal failures
        print(("PASS  " if ok else "FAIL  ") + what)
        if not ok:
            failures += 1

    # The diagnostics by what they were taken for: "before Mario", "mario at
    # spawn", "mario on the stairs", "photo mode", "photo, Mario off", "photo,
    # Mario back", "after Mario".
    labels = {int(m.group(1)): m.group(2) for m in re.finditer(r"host: diagnostics (\d+) \((.*?)\)", host)}
    diags = {}
    for m in re.finditer(r"DIAG (\d+) frame \d+: G-buffer pixels mario (\d+) hero (\d+) pedestrian (\d+) lamp (\d+) \| "
                         r"shadow texels region A \(copied\) (\d+), region B \(not copied\) (\d+) \| hero hidden (\d)"
                         r"(?: \| photo mode's Spider-Man (\d+))?", host):
        diags[labels.get(int(m.group(1)), m.group(1))] = [int(g or 0) for g in m.groups()[1:]]
    reports = {m.group(1): (float(m.group(2)), float(m.group(3)), float(m.group(4)), int(m.group(5)))
               for m in re.finditer(r"REPORT (.+?) hero \(([-\d.]+) ([-\d.]+) ([-\d.]+)\) hidden (\d)", host)}
    report_cams = {m.group(1): (float(m.group(2)), float(m.group(3)), float(m.group(4)))
                   for m in re.finditer(r"REPORT (.+?) hero .*? cam \(([-\d.]+) ([-\d.]+) ([-\d.]+)\)", host)}
    trace = [(float(m.group(1)), float(m.group(2)), float(m.group(3)), float(m.group(4)))
             for m in re.finditer(r"TRACE t ([\d.]+) hero \(([-\d.]+) ([-\d.]+) ([-\d.]+)\)", host)]
    # t, hero (x y z), camera (x y z)
    trace_cam = [tuple(float(g) for g in m.groups())
                 for m in re.finditer(r"TRACE t ([\d.]+) hero \(([-\d.]+) ([-\d.]+) ([-\d.]+)\) cam "
                                      r"\(([-\d.]+) ([-\d.]+) ([-\d.]+)\)", host)]
    cams = {t: (hy, cy) for t, _hx, hy, _hz, _cx, cy, _cz in trace_cam}
    # t, where Mario is on screen (x, y: the centre of his pixels in the G-buffer) and how many pixels
    screen = [(float(m.group(1)), float(m.group(2)), float(m.group(3)), int(m.group(4)))
              for m in re.finditer(r"TRACE t ([\d.]+) hero .*? mario \(([-\d.]+) ([-\d.]+) (\d+)\)", host)]
    override_off = re.search(r"Override\s*=\s*false", os.environ.get("E2E_INI_EXTRA", "")) is not None
    align_off = re.search(r"AlignToCamera\s*=\s*false", os.environ.get("E2E_INI_EXTRA", "")) is not None
    # E2E_CAMERA_THREAD=1: the stand-in writes its camera on a thread of its
    # own, racing the mod's frames (as Spider-Man 2 does on its main thread).
    cam_thread = "host: the camera is written on a thread of its own" in host

    def screen_jitter(t0: float, t1: float) -> float:
        """How much Mario jumps about on screen from frame to frame (90th
        percentile of the second difference of his centre, px): with the
        camera on him he stays put but for his own animation."""
        pts = [p for p in screen if t0 <= p[0] <= t1 and p[3] > 3000]
        d2 = sorted(max(abs(c[1] - 2 * b[1] + a[1]), abs(c[2] - 2 * b[2] + a[2]))
                    for a, b, c in zip(pts, pts[1:], pts[2:]))
        return d2[min(len(d2) - 1, int(0.9 * len(d2)))] if len(d2) >= 8 else 999.0

    # Mario's collision: the game's physics (the default), or the rendered
    # world (0.3's, and the fallback when the physics never answers).
    physics = "collision=the game's physics" in log
    fell_back = re.search(r"collision: (.*) - Mario collides with what the camera sees instead", log)
    game_thread = "host: no game thread" not in host
    world = not physics or fell_back is not None
    print(f"collision: {'the game physics' if physics else 'the rendered world'}"
          f"{' (fell back to the rendered world: ' + fell_back.group(1) + ')' if fell_back else ''}")

    check("host: done" in host, "the host ran to the end")
    check(all(k in diags for k in ("before Mario", "mario at spawn", "mario on the stairs", "photo mode",
                                   "photo, Mario off", "photo, Mario back", "after Mario")),
          "all seven diagnostics ran")
    if "before Mario" in diags:
        mario, hero, *_ = diags["before Mario"]
        check(mario == 0 and hero > 1000, f"before M: Spider-Man drawn ({hero} px), no Mario")
    if "mario at spawn" in diags:
        mario, hero, ped, lamp, shA, shB, hidden, _doppel = diags["mario at spawn"]
        check(mario > 5000, f"Mario is in the game's G-buffer ({mario} px)")
        check(hero == 0 and hidden == 1, "Spider-Man is hidden by the engine and not drawn")
        check(ped > 1000 and lamp > 1000, "the game's own draws around Mario are intact")
        check(shA > 0, f"Mario casts a shadow into the refreshed shadow region ({shA} texels)")
        check(shB == 0, "nothing is drawn into the cached shadow region")
    if "after Mario" in diags:
        mario, hero, *_rest, hidden, _doppel = diags["after Mario"]
        check(mario == 0 and hero > 1000 and hidden == 0, "after M: Spider-Man is back, Mario is gone")

    # The stencil marks on Mario's and Spider-Man's pixels: the stand-in marks
    # everything it draws into the G-buffer 0x01, and Spider-Man also 0x80 in a
    # depth-only pass of his own. The mod learns his mark and gives Mario the
    # same, keeping the G-buffer's bit (0x81).
    marks = {}
    for m in re.finditer(r"DIAG (\d+) frame \d+: .*? \| stencil mario (0x[0-9a-f]{2}|-) \((\d+)%\) "
                         r"hero (0x[0-9a-f]{2}|-) \((\d+)%\)", host):
        marks[labels.get(int(m.group(1)), m.group(1))] = (m.group(2), int(m.group(3)), m.group(4), int(m.group(5)))
    decided = re.search(r"stencil: Spider-Man's pixels are marked (.*?) - Mario's get the same(.*?): "
                        r"(0x[0-9a-f]{2}) under mask (0x[0-9a-f]{2})", log)
    if "before Mario" in marks:
        _m, _ms, h, hs = marks["before Mario"]
        check(h == "0x81" and hs >= 95, f"before M: Spider-Man's pixels are marked 0x81 ({h}, {hs}%)")
    check(decided is not None and decided.group(1).startswith("0x80 under mask 0x80") and
          decided.group(3) == "0x81" and decided.group(4) == "0x81",
          "stencil: the mod found Spider-Man's own mark (0x80) and gives Mario it and the G-buffer's (0x81)" +
          (f" ({decided.group(0)[:160]})" if decided else ""))
    for label in ("mario on the stairs", "photo mode"):
        if label in marks:
            mm, ms, _h, _hs = marks[label]
            check(mm == "0x81" and ms >= 95, f"{label}: Mario's pixels are marked like Spider-Man's ({mm}, {ms}%)")
    # The game hid Spider-Man itself just before M, then tried to show him
    # while Mario stood in (refused): when Mario goes, he must be shown.
    check("the game hides Spider-Man (a gadget) - hidden 1" in host, "the game hid Spider-Man itself before M")
    unhides = re.search(r"the game called Unhide on Spider-Man (\d+) time\(s\) while Mario was on; he showed (\d+)", host)
    check(unhides is not None and int(unhides.group(1)) >= 3 and int(unhides.group(2)) == 0,
          "the game's Transform::Unhide never showed Spider-Man while Mario stood in" +
          (f" ({unhides.group(1)} calls, shown {unhides.group(2)})" if unhides else ""))
    check(re.search(r"hero: the game tried to show Spider-Man \d+ time\(s\) - kept hidden", log) is not None,
          "the mod logged the refused Unhide calls")

    # (before the strafe at 15 s: with a camera that lags, "right" turns as
    # the camera swings round, and the strafe curves back over z -27.5)
    on_platform = [t for t in trace if -35.0 < t[3] < -27.5 and t[0] < 14.9]
    check(bool(on_platform) and all(abs(t[2] - 6.0) < 0.05 for t in on_platform),
          "Mario climbed the stairs and walked on the 1 m platform (y = 6.0)")
    rh = "screen right-handed" in host
    before = [tr for tr in trace if 14.9 <= tr[0] <= 15.1]
    after = [tr for tr in trace if tr[0] >= 16.3]
    if before and after:
        dx = (after[0][1] - before[0][1]) * (-1.0 if rh else 1.0) # world x of screen right: -x / +x
        check(dx > 1.0, f"D moves Mario to the right on screen ({dx:.2f} m, {'right' if rh else 'left'}-handed screen)")
    else:
        check(False, "D moves Mario to the right on screen (no trace)")
    jump = sorted((t, hy, cy) for t, (hy, cy) in cams.items() if 24.3 <= t <= 25.9)
    peak = max((hy for _, hy, _ in jump), default=0.0)
    check(peak - 5.0 > 0.8, f"Mario jumped at the wall ({peak - 5.0:.2f} m)")
    # The mock's follow camera sits 2.2 m above and 4 m behind its target, and
    # trails it through a 0.35 s spring (like the game's: 0.4's log had 0.6 s).
    # The mod places it [Camera] Distance times as far (E2E_CAMERA_DISTANCE).
    lags = re.findall(r"camera lead: the game camera trails its target by ([\d.]+) s", log)
    lag = lags[-1] if lags else None
    pinned = "hero pin: Spider-Man is held on Mario through the game's frame" in log
    placed = "camera: placed by the mod" in log
    dist = float(os.environ.get("E2E_CAMERA_DISTANCE", "1.0")) if pinned and not override_off else 1.0
    UP, BACK = 2.2 * dist, 4.0 * dist
    cam_peak = max((cy for _, _, cy in jump), default=0.0)
    worst = max((abs(cy - hy - UP) for _, hy, cy in jump), default=99.0)
    after = [cy - hy for t, hy, cy in jump if t >= 25.6]
    if pinned and not override_off:
        # The mod places the camera: where the game's own sits at rest, around
        # Mario wherever he is, aimed by the game.
        check("camera: the game's camera is the transform written each frame where it renders from" in log,
              "camera override: the game's camera transform found (written each frame where the view comes from)")
        check(placed, "camera override: the mod placed the camera (learnt the game's framing while Mario stood)")
        walk = [x for x in trace_cam if 15.0 <= x[0] <= 16.6]
        dx = max((abs(cx - hx) for _t, hx, _hy, _hz, cx, _cy, _cz in walk), default=99.0)
        dz = max((abs(cz - hz + BACK) for _t, _hx, _hy, hz, _cx, _cy, cz in walk), default=99.0)
        dy = max((abs(cy - hy - UP) for _t, _hx, hy, _hz, _cx, cy, _cz in walk), default=99.0)
        rays = physics and not fell_back
        # With the camera written on another thread it takes Mario's place as
        # it was a frame before the write: up to a frame of his motion behind
        # where he was put last (the trace's), and Mario is drawn there too.
        walk_step = max((abs(b[1] - a[1]) + abs(b[3] - a[3]) for a, b in zip(walk, walk[1:])), default=0.0)
        jump_step = max((abs(b[1] - a[1]) for a, b in zip(jump, jump[1:])), default=0.0)
        slack = 1.2 * walk_step if cam_thread else 0.0
        # (Without the game's physics there are no camera rays: the game's own
        # camera distance limits it, and that one comes closer as it lags.)
        check(bool(walk) and dx < 0.05 + slack and (not rays or (dz < 0.1 + slack and dy < 0.1)),
              f"walking, the camera kept its place behind Mario - no lag (off by up to {dx:.2f} m across, {dz:.2f} m "
              f"back, {dy:.2f} m up{'' if rays else '; closer allowed: no camera rays'}"
              f"{f'; up to a frame of his motion behind - {walk_step:.2f} m - with the camera on its own thread' if cam_thread else ''})")
        # (Its height eases after his by VerticalSmoothing, 0.05 s since 0.6:
        # 0.22-0.31 m behind at worst as he takes off at 8 m/s, depending on
        # which frames this mock renders - 0.5's 0.15 s gave 0.71-0.81 m and a
        # rise of 2.06-2.11 m for his 2.4. Without camera rays, the game's own
        # camera distance limits it too, and that one shrinks as the game's
        # camera lags behind the jump: 0.59-0.60 m (0.5: 1.11 m). The game's own
        # camera alone rises 65%.)
        jump_slack = 1.2 * jump_step if cam_thread else 0.0
        check(bool(jump) and cam_peak - (5.0 + UP) > 0.9 * (peak - 5.0) and
              worst < (0.45 if rays else 0.8) + jump_slack and bool(after) and all(abs(x - UP) < 0.1 for x in after),
              f"the camera followed Mario up ({cam_peak - (5.0 + UP):.2f} m) and back down (worst offset {worst:.2f} m "
              f"from {UP:.1f} m above him, {after[-1] if after else 0:.2f} m after landing"
              f"{f'; a frame of his motion allowed on its own thread: {jump_step:.2f} m' if cam_thread else ''})")
        # On screen: Mario stays put while the camera follows him, but for his
        # own animation (the stand-in's frames are slow and its Mario small: a
        # 90th percentile of 20-30 px). 0.6 with the camera on another thread:
        # Mario jumped a frame of his motion about against his camera - 65-75 px
        # (60 px is a frame of walking here).
        # (With AlignToCamera = false and the camera on its own thread - the
        # negative control - this one fails: 65 and 75 px.)
        # (0.6.0 and 0.6.1's first builds eased the camera's height by a clock
        # read after the frame boundary's work, which took longer in some
        # frames: 50-80 px jumping with the rendered-world collision, whose
        # frame boundary has the depth to read back.)
        jw, jj = screen_jitter(15.1, 16.4), screen_jitter(24.3, 25.9)
        check(jw < 45 and jj < 45,
              f"on screen Mario kept his place with the camera on him (jumping about {jw:.0f} px walking, "
              f"{jj:.0f} px jumping - his animation{'; the camera written on another thread' if cam_thread else ''})")
        if not align_off:
            al = re.findall(r"camera: Mario drawn where the camera of the view had him - (\d+) views read back: the "
                            r"view's own placement found in (\d+)%", log)
            check(bool(al) and int(al[0][0]) > 100 and int(al[0][1]) >= 90,
                  "Mario was drawn where the camera of each view had him (the view's placement found by the draw)" +
                  (f" ({al[0][0]} views, {al[0][1]}%)" if al else ""))
        if cam_thread:
            race = re.findall(r"against the camera the mod placed \((\d+) frames\): in step (\d+)%, a frame ahead (\d+)%",
                              log)
            check(bool(race) and int(race[0][2]) >= 10,
                  "the race: the camera came from the mod's frame before Mario's in some frames" +
                  (f" ({race[0][2]}% of {race[0][0]})" if race else ""))
            th = re.search(r"on thread (\d+) \(the mod's frames on thread (\d+): Mario's place as it was a frame "
                           r"before, for (\d+) write\(s\)\)", log)
            check(th is not None and th.group(1) != th.group(2) and int(th.group(3)) > 100,
                  "camera writes on the other thread took Mario's place as it was a frame before" +
                  (f" (thread {th.group(1)}, the mod's {th.group(2)}, {th.group(3)} writes)" if th else ""))
        # The game's camera became another transform (as after a load) while
        # Mario jumped: the old one stopped being written as the view moved on
        # - forgotten, the new one found, its framing learnt as he stood.
        lost = log.find("camera: the game stopped writing that transform while its view moved on")
        found = log.find("camera: the game's camera is the transform written each frame", lost) if lost >= 0 else -1
        again = log.find("camera: placed by the mod", found) if found >= 0 else -1
        # (Or, since 0.6.1, when it had seen the game render from the new one
        # and placed it too already: that one took over, without a gap.)
        took_over = "that one is the game's camera now" in log
        check((lost >= 0 and found >= 0 and again >= 0) or took_over,
              "the game's camera became a new transform: the mod noticed and placed the new one "
              f"(lost {lost >= 0}, found again {found >= 0}, placed {again >= 0}; took over from the views {took_over})")
        if physics and not fell_back:
            # A wall only the camera's query sees, put between Mario (at the
            # wall) and his camera: the camera comes in front of it.
            wall_h, wall_c = reports.get("at the wall"), report_cams.get("at the wall")
            blk_h, blk_c = reports.get("camera blocked"), report_cams.get("camera blocked")
            check(wall_h is not None and wall_c is not None and abs(wall_c[2] - (wall_h[2] - BACK)) < 0.15,
                  f"nothing in the way: the camera {BACK:.1f} m behind Mario" +
                  (f" ({wall_c[2] - wall_h[2]:.2f} m)" if wall_c and wall_h else ""))
            check(blk_h is not None and blk_c is not None and -26.85 < blk_c[2] < blk_h[2] - 0.3,
                  "a wall between Mario and the camera: it came in front of the wall (z -26.9)" +
                  (f" (at z {blk_c[2]:.2f}, Mario at {blk_h[2]:.2f})" if blk_c and blk_h else ""))
            check("camera: walls from the game's physics (query 11)" in log, "camera walls from the game's camera query")
            rays = re.search(r"camera rays: (\d+) \((\d+) hit something\)", host)
            check(rays is not None and int(rays.group(1)) > 100 and int(rays.group(2)) > 0,
                  "the camera's rays were cast and hit the wall" + (f" ({rays.group(1)}, {rays.group(2)} hit)" if rays else ""))
    else:
        check(not placed, "the camera is the game's own (not placed by the mod)")
        if pinned:
            # 0.4's way: its target led along Mario's jumps by the camera's lag.
            check(lag is not None and 0.2 < float(lag) < 0.6,
                  "the camera lead learnt the camera's lag (a 0.35 s spring)" + (f" ({lag} s)" if lag else ""))
            # (the first jump: the lead is still learning, the game's camera trails)
            # (After landing it eases back down through its spring: 0.5 m above
            # its place a quarter of a second after he lands, in a fast run.)
            check(bool(jump) and cam_peak - 7.2 > 0.5 * (peak - 5.0) and bool(after) and
                  all(abs(x - 2.2) < 0.6 for x in after) and abs(after[-1] - 2.2) < 0.4,
                  f"the game camera followed Mario up ({cam_peak - 7.2:.2f} m) and back down "
                  f"({after[-1] if after else 0:.2f} m above him after landing)")
        else:
            check(lag is None, "without the pin the camera lead stays out of it")
            check(bool(jump) and cam_peak - 7.2 > 0.6 * (peak - 5.0),
                  f"the game camera followed Mario up ({cam_peak - 7.2:.2f} m) - expected to fail without the pin")
    snaps = re.search(r"put the hero back on the ground (\d+) time", host)
    check(snaps is not None and int(snaps.group(1)) > 0, "the game's character controller fought the pin")
    check("hero pin: Spider-Man is held on Mario through the game's frame (hooked SetPosition, SetMatrix, SetMatrixEx, "
          "SetMatrixEx2, MarkDirty)" in log, "hero pin hooks installed (all five setters)")
    # The game's SetMatrixEx2 takes six arguments, the sixth a pointer it reads:
    # 0.5.0's first build passed five, and the game crashed at start-up.
    ex = re.search(r"SetMatrixEx called (\d+) time\(s\), (\d+) left the wrong thing; SetMatrixEx2 called (\d+) time\(s\), "
                   r"its sixth argument lost (\d+)", host)
    check(ex is not None and int(ex.group(1)) > 100 and int(ex.group(2)) == 0 and int(ex.group(3)) > 100 and
          int(ex.group(4)) == 0,
          "the game's SetMatrixEx (4 arguments) and SetMatrixEx2 (6) got every argument through the hooks" +
          (f" ({ex.group(1)} / {ex.group(3)} calls, {ex.group(2)} / {ex.group(4)} wrong)" if ex else ""))
    check("camera: the game's camera target follows Mario" in log, "camera target hooks installed (found by RTTI)")
    undone = re.search(r"the game moved Spider-Man off Mario (\d+) time", log)
    check(undone is not None and int(undone.group(1)) > 0, "the pin undid the controller's moves" +
          (f" ({undone.group(1)})" if undone else ""))
    wall = reports.get("at the wall")
    # (The rendered world knows the wall to within its cells: 0.25 m.)
    tol = 0.06 if physics and not fell_back else 0.3
    check(wall is not None and abs(wall[2] + 24.5) < tol and abs(wall[1] - 5.0) < 0.06,
          f"Mario stopped against the wall, on the ground (at {wall[:3] if wall else None})")
    inside = [t for t in trace if t[3] > -24.4 and t[2] < 12.95]
    check(not inside, "Mario never went into the wall (face at z = -24, radius 0.5; top at y = 13)" +
          (f": {inside[0]}" if inside else ""))

    cars = re.search(r"a car drove by in (\d+) frame", host)
    check(cars is not None and int(cars.group(1)) >= 5, "a car drove across Mario's way" +
          (f" ({cars.group(1)} frames)" if cars else ""))
    # ... and left nothing behind: he walks on into its lane (z -26.6..-24.6), past its middle (a wall
    # left where the car was would stop him at z -27.1). (How soon he gets there depends on how
    # quickly the stairs ahead are answered: up to 1.3 s on the stairs in a slow run.)
    through = [t for t in trace if 12.6 <= t[0] <= 16.6 and t[3] > -25.6]
    check(bool(through), "Mario walked on through the lane the car had just driven along" +
          (f" (at {through[0][0]:.2f} s)" if through else ""))
    check("Mario is drawn inside the game's frame" in log, "the mod drew Mario inside the frame")
    if world:
        check("motion vectors are in use" in log, "collision uses the game's motion vectors (moving things aren't solid)")
        check("first depth frame from the game for collision" in log, "collision came from the game's depth")
    if physics and not game_thread:
        check(fell_back is not None and "hasn't run since M" in fell_back.group(1),
              "the physics frame never ran: Mario fell back to the rendered world")
    if physics and game_thread:
        check(fell_back is None, "Mario stayed on the game's physics")
        check(re.search(r"game physics: ray cast exe\+0x[0-9a-f]+, physics exe\+0x[0-9a-f]+, frame hook exe\+0x[0-9a-f]+, "
                        r"requests exe\+0x[0-9a-f]+/exe\+0x[0-9a-f]+, ignore hero yes, hit actors yes, query pool 256 per "
                        r"frame, pause detection yes, damage system exe\+0x[0-9a-f]+", log) is not None,
              "game physics: every binding resolved")
        check("collision: the game's physics (" in log, "collision switched to the game's physics after M")
        answered = re.search(r"the game answered (\d+) rays \((\d+) hits, (\d+) empty\) in (\d+) frames.* on thread (\d+); "
                             r"(\d+) waited for the game's queries; most query results in use (-?\d+)", log)
        check(answered is not None and int(answered.group(1)) > 1000 and int(answered.group(2)) > 0,
              "the game answered Mario's rays" + (f" ({answered.group(1)} rays, {answered.group(2)} hits)" if answered else ""))
        check(answered is not None and int(answered.group(6)) > 0 and int(answered.group(7)) >= 190,
              "rays waited while the game used its query pool" + (f" ({answered.group(6)})" if answered else ""))
        ph = re.search(r"physics: (\d+) frames, (\d+) steps \((\d+) with the query pool busy\), (\d+) ray casts on the game "
                       r"thread, (\d+) elsewhere, (\d+) water, (\d+) bad, (\d+) untagged, (\d+) with the pool full; the hero "
                       r"left out of (\d+), hit by (\d+); requests still held (-?\d+)", host)
        if ph:
            g = [int(x) for x in ph.groups()]
            check(g[3] > 1000 and g[4] == 0, f"rays were cast on the game's thread only ({g[3]}, {g[4]} elsewhere)")
            check(g[5] > 0, f"water rays asked ({g[5]})")
            check(g[6] == 0 and g[7] == 0 and g[8] == 0, f"every ray well formed (bad {g[6]}, untagged {g[7]}, pool full {g[8]})")
            check(g[9] > 0 and g[10] == 0, f"Spider-Man was left out of the rays (left out of {g[9]}, hit by {g[10]})")
            check(g[11] == 0, f"every request was released ({g[11]} held)")
        else:
            check(False, "the host's physics summary")
        for mat, steps in (("kAsphalt", "stone"), ("kConcrete", "stone"), ("kGrass", "grass")):
            check(f"collision: Mario walks on {mat} ({steps} footsteps)" in log, f"{mat} under Mario: {steps} footsteps")
        check("collision: 'ped_mock_tourist' is not solid for Mario (a person)" in log, "the pedestrian isn't solid")
        check("collision: 'enemy_mock_thug' is not solid for Mario (a person)" in log, "the thug isn't solid")
    # Pausing the game pauses Mario (the physics frame tells).
    if game_thread:
        check("the game is paused - so is Mario" in log and "the game is running again - so is Mario" in log,
              "the mod saw the game pause and run again")
        pa, pb = reports.get("paused a"), reports.get("paused b")
        check(pa is not None and pb is not None and all(abs(pa[i] - pb[i]) < 0.02 for i in range(3)),
              f"Mario stood still while the game was paused, walk key held ({pa[:3] if pa else None} -> {pb[:3] if pb else None})")
        # Hits through the game's damage system.
        dmg = [m.groupdict() for m in re.finditer(
            r"DAMAGE (?:actor 0x(?P<actor>[0-9a-f]+)|sphere \((?P<c>[-\d. ]+)\) r (?P<r>[\d.]+) query (?P<query>\d+)"
            r"(?P<released>.*?)) \| damager 0x(?P<damager>[0-9a-f]+) type (?P<type>\d+) amount (?P<amount>[\d.]+) "
            r"knockback (?P<kb>\d+) x(?P<kbx>[\d.]+) flags 0x(?P<flags>[0-9a-f]+) masks 0x(?P<m1>[0-9a-f]+) "
            r"0x(?P<m2>[0-9a-f]+) \| (?P<who>.*)", host)]
        thug = [d for d in dmg if d["who"] == "the thug" and float(d["amount"]) > 0]
        # (the thug: serial 9, slot 5 - found by his MockGuardHealth, a Health)
        check(len(thug) == 1 and thug[0]["actor"] == "900005" and thug[0]["damager"] == "700003" and
              thug[0]["type"] == "1" and float(thug[0]["amount"]) == 100.0 and thug[0]["kb"] == "2" and
              abs(float(thug[0]["kbx"]) - 100.0) < 1e-3 and thug[0]["flags"] == "22000" and
              thug[0]["m1"] == "99600" and thug[0]["m2"] == "99600",
              "the punch hit the thug through the game's damage system, by his handle (melee 100, stagger x100, "
              "from Spider-Man)" + (f": {thug}" if len(thug) != 1 else ""))
        people = [d for d in dmg if float(d["amount"]) == 0.0]
        check(len(people) >= 1 and people[0]["query"] is not None and people[0]["released"] == "" and
              people[0]["flags"] == "40032008" and abs(float(people[0]["kbx"]) - 100.0) < 1e-3,
              "a punch at nobody asks the game to make people in reach react, unhurt" + (f": {people[:1]}" if people else ""))
        check("combat: someone to fight near Mario (slot 5, health 500/500, 'enemy_mock_thug')" in log,
              "the thug was found as someone to fight (his MockGuardHealth derives from Health)")
        check("hit an enemy 'enemy_mock_thug' with punch: 100 damage (500 of 500 left), stagger" in log,
              "the mod logged the hit")
        check("the thug hit 1 time(s), his health 400" in host, "the game dealt the damage")
    else:
        check("hit 'enemy_mock_thug' with punch: 500 -> 400 of 500" in log,
              "without the game's damage system the punch takes health directly")
    # The game keeps the controls Mario doesn't use.
    check("interaction: Mario answers the pedestrian" in log, "the interact key: Mario answers the pedestrian")
    keys = re.search(r"the game got the interact key (\d+) time\(s\), Mario's own keys (\d+) time\(s\)", host)
    check(keys is not None and int(keys.group(1)) >= 1 and int(keys.group(2)) == 0,
          "the game still gets its interact key, but none of Mario's" + (f" ({keys.group(1)}, {keys.group(2)})" if keys else ""))
    crashes = [l for l in log.splitlines() if "CRASH PREVENTED" in l and "audio init" not in l]
    check(not crashes, "no prevented crashes (besides Wine's XAudio2)" + (": " + crashes[0] if crashes else ""))
    check("fell out of the world" not in log and "fell for too long" not in log, "Mario never fell out of the world")

    # Poses: a pedestrian asking for a picture, and the pose key.
    check("photo poses: watching for pedestrians who ask for a picture" in log, "photo request hook installed (found by RTTI)")
    check("a pedestrian asks for a picture (behaviour entered 1 time" in host, "the pedestrian's behaviour ran (hook called through)")
    check(re.search(r"photo request: a pedestrian [\d.]+ m away wants a picture", log) is not None,
          "the mod saw the pedestrian's request")
    check("photo request: Mario poses (wave)" in log, "Mario waved for the pedestrian's picture")
    check("pose key: Mario poses (wave)" in log, "the pose key made Mario pose")

    # Mario's own menu (F8).
    check("settings (F8 menu): MARIO VOLUME -> 85%" in log, "F8 menu: the volume went up a step")
    check("settings (F8 menu): MOON JUMP -> ON" in log, "F8 menu: MOON JUMP switched on")
    check("settings saved to sm2mario.ini (menu closed)" in log, "F8 menu: settings saved when it closed")
    leaked = re.search(r"the game got (\d+) key-down\(s\) of Mario Mode's menu/pose keys", host)
    check(leaked is not None and int(leaked.group(1)) == 0,
          "the game never saw F8, P, or the menu's Esc/Enter/arrows" + (f" ({leaked.group(1)} seen)" if leaked else ""))

    # ModSettings' pause menu.
    if "ModSettings mock loaded" in host:
        check("ModSettings: Mario Mode page added to the pause menu" in log, "ModSettings: page registered")
        opens = re.findall(r"MODSETTINGS open: (\d+) page\(s\): (.*)", host)
        check(len(opens) == 2 and all(n == "1" for n, _ in opens), "ModSettings: one page, built twice")
        if opens:
            first = opens[0][1]
            check("page 'MARIO MODE' (type 0," in first and "with description" in first and
                  "3 headers, 8 toggles, 0 options, 4 sliders" in first, "ModSettings: the page's items (" + first[:90] + "...)")
            check("MOON JUMP=1" in first and "MARIO VOLUME=0.85[0..1|0..20]" in first and "INFINITE HEALTH=0" in first,
                  "ModSettings: the page shows the F8 menu's changes")
            check("ERRORS" not in first, "ModSettings: every item well formed")
            check("rtti .?AVMarioModeMenu@@ = .?AVMarioModeMenu@@ : .?AVModMenu@@ : .?AVBaseMenu@@;" in first,
                  "ModSettings: the page has MSVC run-time type information (typeid / dynamic_cast<ModMenu*> work)")
        if len(opens) > 1:
            check("INFINITE HEALTH=1" in opens[1][1], "ModSettings: the change stuck")
        check("settings (pause menu): INFINITE HEALTH -> ON" in log, "ModSettings: the change reached Mario Mode")
        check("settings saved to sm2mario.ini (ModSettings saved)" in log, "ModSettings: saved on its request")
        calls = re.search(r"MODSETTINGS callbacks called (\d+), type checks (\d+)", host)
        check(calls is not None and int(calls.group(1)) == 2 and calls.group(1) == calls.group(2),
              "ModSettings: the std::function callbacks worked the MSVC way" + (f" ({calls.group(1)} calls)" if calls else ""))
    user = (game / "sm2mario" / "sm2mario.ini").read_text(errors="replace")
    def ini_has(section: str, key: str, value: str) -> bool:
        m = re.search(r"\[" + section + r"\]([^\[]*)", user)
        return m is not None and re.search(r"(?m)^\s*" + key + r"\s*=\s*" + re.escape(value) + r"\b", m.group(1)) is not None
    check(ini_has("Cheats", "MoonJump", "true") and ini_has("Audio", "Volume", "0.85"),
          "the user's sm2mario.ini holds the menu's settings")
    if "ModSettings mock loaded" in host:
        check(ini_has("Cheats", "InfiniteHealth", "true"), "... and ModSettings' change")

    # The game's photo mode.
    check(re.search(r"photo mode: watching the game's photo mode \(PhotomodeSystem @exe\+0x[0-9a-f]+", log) is not None,
          "photo mode found (PhotomodeSystem, by the selfie switch's pattern)")
    check("photo mode: open - Mario holds still for the shot" in log and "photo mode: closed - Mario carries on" in log,
          "the mod saw photo mode open and close")
    po, pw = reports.get("photo open"), reports.get("photo walked")
    check(po is not None and pw is not None and all(abs(po[i] - pw[i]) < 0.01 for i in range(3)),
          f"in photo mode Mario stood still while W moved the game's photo camera ({po[:3] if po else None} -> "
          f"{pw[:3] if pw else None})")
    pk = re.search(r"in photo mode the game got (\d+) of Mario's keys", host)
    check(pk is not None and int(pk.group(1)) > 0, "... and the game got those keys" + (f" ({pk.group(1)})" if pk else ""))
    check("pose key: Mario poses (peace sign)" in log and "photo mode: Mario holds the peace sign for the shot" in log,
          "in photo mode the pose key made Mario strike a pose and hold it")
    check("photo mode: the selfie's copy of Spider-Man is hidden - Mario is the one in the shot" in log,
          "the selfie's copy of Spider-Man was hidden")
    if "photo mode" in diags:
        mario, hero, *_rest, hidden, doppel = diags["photo mode"]
        check(mario > 5000 and hidden == 1 and doppel == 0,
              f"photo mode: Mario in the frame ({mario} px), Spider-Man hidden, no selfie copy of him ({doppel} px)")
    if "photo, Mario off" in diags:
        # (In a selfie the game hides the hero by a flag of its own - read from
        # its selfie code; the mock does the same - so he isn't drawn either
        # way: what counts is that the mod gave his transform back, since the
        # game had hidden him before M and tried to show him while Mario was
        # out.)
        mario, _hero, *_rest, hidden, doppel = diags["photo, Mario off"]
        check(mario == 0 and doppel > 1000 and hidden == 0,
              f"Mario Mode off in a selfie: the game's copy of Spider-Man is back ({doppel} px), Mario gone ({mario} px), "
              f"the hero given back to the game (hidden {hidden})")
    if "photo, Mario back" in diags:
        mario, _hero, *_rest, hidden, doppel = diags["photo, Mario back"]
        check(mario > 5000 and doppel == 0 and hidden == 1,
              f"... and on again: Mario in the shot ({mario} px), the copy hidden again ({doppel} px)")

    # Moon jump: holding jump keeps Mario rising.
    moon = [t for t in trace if 53.7 <= t[0] <= 57.2]
    top = max((t[2] for t in moon), default=0.0)
    check(top - 5.0 > 5.0, f"moon jump: holding jump took Mario {top - 5.0:.1f} m up (a jump is ~1.5 m)")
    # E2E_CAMERA_FAR=1: after his jumps at the wall the game's own camera falls
    # 50 m behind him (as when Mario flies or falls faster than its spring),
    # still looking at him. 0.6 let go of the camera once the game's was 25 m
    # away (and it compared each write with the view it had placed: more than
    # 40 m off, it left the write to the game).
    if "host: the game's own camera falls 50 m behind Mario" in host and pinned and not override_off:
        hold = [x for x in trace_cam if 34.4 <= x[0] <= 35.3]
        back = [hz - cz for _t, _hx, _hy, hz, _cx, _cy, cz in hold]
        check(len(hold) >= 5 and all(abs(b - BACK) < 0.6 for b in back),
              "the game's own camera 50 m behind Mario: the mod kept placing it behind him, every frame" +
              (f" ({min(back):.2f}-{max(back):.2f} m behind, {len(hold)} frames)" if back else ""))
    # E2E_CAMERA_RIGS=1: the game's other camera - 3 m further back, 1 m higher,
    # aimed a degree lower - rendered from in every other frame from 21.0 to
    # 27.5 s (0.6 in flight: a fifth of the frames from a camera it didn't
    # place - the camera snapped close and far). The mod must see views come
    # from it, find its transform and place it too.
    place_off = re.search(r"PlaceOtherCameras\s*=\s*false", os.environ.get("E2E_INI_EXTRA", "")) is not None
    if "host: the game has another camera" in host and pinned and not override_off:
        rig_frames = re.search(r"host: (\d+) frame\(s\) rendered from the game's other camera", host)
        adopted = log.find("camera: the game renders from another of its cameras too")
        if place_off:
            check(rig_frames is not None and int(rig_frames.group(1)) > 20 and adopted < 0,
                  "PlaceOtherCameras = false: the game's other camera was left to the game")
        else:
            check(rig_frames is not None and int(rig_frames.group(1)) > 20 and adopted >= 0,
                  "the game's other camera: the mod saw views come from it and placed it too" +
                  (f" ({rig_frames.group(1)} frames rendered from it)" if rig_frames else ""))
            # From then on whichever camera the game renders from sits behind
            # Mario: where it is against him changes smoothly (a second
            # difference of ~3 m every other frame without it - the close and
            # far snapping).
            win = [x for x in trace_cam if 22.0 <= x[0] <= 27.4]
            rel = [(cx - hx, cy - hy, cz - hz) for _t, hx, hy, hz, cx, cy, cz in win]
            d2 = sorted(max(abs(c[i] - 2 * b[i] + a[i]) for i in range(3)) for a, b, c in zip(rel, rel[1:], rel[2:]))
            snap = d2[min(len(d2) - 1, int(0.9 * len(d2)))] if len(d2) >= 8 else 99.0
            # (With the camera written on another thread, it is a frame of
            # Mario's motion behind him or not, by the race: up to twice that
            # in the second difference.)
            moves = sorted(max(abs(b[i] - a[i]) for i in (1, 2, 3)) for a, b in zip(win, win[1:]))
            step = moves[int(0.9 * (len(moves) - 1))] if moves else 0.0
            snap_limit = 0.25 + (2.0 * step if cam_thread else 0.0)
            check(len(win) >= 20 and snap < snap_limit,
                  f"... and from then on the camera kept its place behind Mario whichever one the game rendered from "
                  f"(it jumped about {snap:.2f} m, {len(win)} frames"
                  f"{f'; up to {snap_limit:.2f} m with the camera on its own thread' if cam_thread else ''})")
            jr = screen_jitter(22.0, 27.4)
            check(jr < 45, f"... and Mario kept his place on screen ({jr:.0f} px)")
            most = [int(x) for x in re.findall(r"the game's cameras placed: up to (\d+) at once", log)]
            check(bool(most) and max(most) >= 2,
                  "the summary counted the cameras placed" + (f" (up to {max(most)} at once)" if most else ""))
        # Either way, Mario's motion vectors in the frames where the game
        # switched cameras: his own motion (0.6: the jump between the cameras
        # too - the game's motion blur smeared him along it, a blocky trail).
        mvs = [(float(m.group(1)), int(m.group(2)), float(m.group(3)), int(m.group(4)))
               for m in re.finditer(r"TRACE t ([\d.]+) hero .*? mario \([-\d.]+ [-\d.]+ (\d+)\) mv ([\d.]+) rig (\d)", host)]
        switched = [b[2] for a, b in zip(mvs, mvs[1:]) if 21.3 <= b[0] <= 27.4 and b[1] > 3000 and a[3] != b[3]]
        walking = [x[2] for x in mvs if 15.1 <= x[0] <= 16.4 and x[1] > 3000]
        def median(xs):
            return sorted(xs)[len(xs) // 2] if xs else 999.0
        print(f"info: Mario's motion vectors - walking {median(walking):.1f} px, as the game switched cameras "
              f"{median(switched):.1f} px (median of {len(switched)}; max {max(switched, default=0):.1f})")
        check(len(switched) >= 20 and median(switched) < 8.0,
              f"as the game switched cameras Mario's motion vectors were his own motion (median {median(switched):.1f} px, "
              f"walking {median(walking):.1f} px) - nothing for its motion blur to smear")
    landed = reports.get("after the moon jump")
    if physics and game_thread:
        # The game's physics knows the top of the 8 m wall he rose past: falling
        # back along it, he catches its edge (SM64's ledge grab) - or lands.
        check(landed is not None and (abs(landed[1] - 5.0) < 0.06 or abs(landed[1] - 13.0) < 0.06),
              f"... and he came back down, or caught the top of the wall (at {landed[:3] if landed else None})")
    else:
        check(landed is not None and abs(landed[1] - 5.0) < 0.06, f"... and he came back down (at {landed[:3] if landed else None})")

    # While the game's camera changed transform, Mario jumped at the wall (on
    # the ground in between).
    swap = [t for t in trace if 28.9 <= t[0] <= 36.6]
    check(bool(swap) and min(t[2] for t in swap) > 4.94 and max(t[2] for t in swap) - 5.0 > 1.0,
          "while the camera changed, Mario jumped at the wall and stayed on the ground in between" +
          (f" (lowest {min(t[2] for t in swap):.2f}, highest {max(t[2] for t in swap):.2f})" if swap else ""))

    # The game's component registry (a type's ComponentInfo by its name's CRC,
    # as the real game looks components up): the thug's health was matched as
    # a type derived from Health through it.
    reg = re.search(r"component registry: (\d+) lookup\(s\), (\d+) answered", host)
    if "the component registry answers nothing" in host:
        check(reg is not None and int(reg.group(2)) == 0, "the component registry answered nothing (lookups by name)")
    else:
        check(reg is not None and int(reg.group(1)) > 0 and int(reg.group(2)) > 0,
              "components were looked up through the game's registry" + (f" ({reg.group(1)}, {reg.group(2)} answered)" if reg else ""))

    print(f"\n{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
