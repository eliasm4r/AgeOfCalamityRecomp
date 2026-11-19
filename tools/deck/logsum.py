#!/usr/bin/env python3
"""Sums up a runtime log (logs/game.log of a played session, or the run.log of a measurement): fps, power, temperature,
GPU, cost of the pool shadows and of write watching, audio and errors, per stretch of the session.

Usage: logsum.py <log> [--from <frame>] [--to <frame>] [--rows]
       logsum.py --list <log> [<log> ...]

Without --from/--to the summary covers the whole log and, apart, the heaviest stretch (the 60 s window with the lowest
fps). The "sys:" lines (runtime/sysmon.c, always on under Linux) give power and temperature; the "perf:", "mem:" and
"fps:" ones only exist with ACR_TRACE=perf. --rows lists the "sys:" lines one by one. --list gives one line per session
(date, length, fps, watts, temperatures, errors) to compare sessions. Also reads compressed logs (.gz, the ones in
logs/sessions/ on the Deck).
"""
import argparse
import gzip
import os
import re
import sys
import time

NUM = r'(-?[0-9]+(?:\.[0-9]+)?)'


def grab(pattern, text, cast=float):
    m = re.search(pattern, text)
    return cast(m.group(1)) if m else None


def parse(path):
    sysl, fps, shadow, ww, perf, gpu, ft, audio, bad, info = [], [], [], [], [], [], [], [], [], []
    last_shadow_frame = None
    started = None
    opener = gzip.open if path.endswith('.gz') else open
    with opener(path, 'rt', encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if not line.startswith('[host] '):
                continue
            t = line[7:]
            if started is None and 'gained the focus at ' in t:
                started = t.split('gained the focus at ')[1].replace(' local time', '').strip()[:16]
            if t.startswith('sys: ') and ' to frame ' in t:
                sysl.append({
                    'secs': grab(NUM + r' s to frame', t), 'frame': grab(r'to frame ([0-9]+)', t, int),
                    'fps': grab(r': ' + NUM + r' fps', t), 'apu': grab(r'APU ' + NUM + r' W', t),
                    'apu_peak': grab(r'APU [0-9.]+ W \(peak ' + NUM, t), 'bat': grab(r'battery ' + NUM + r' W', t),
                    'bat_state': (re.search(r'battery [0-9.]+ W ([A-Za-z ]+),', t) or [None, None])[1],
                    'bat_pct': grab(r', ([0-9]+)%', t, int), 'cpu_c': grab(r'cpu ' + NUM + r' C', t),
                    'cpu_c_peak': grab(r'cpu [0-9.]+ C \(peak ' + NUM, t), 'gpu_c': grab(r'gpu ' + NUM + r' C', t),
                    'fan': grab(r'fan ' + NUM + r' rpm', t), 'gpu_busy': grab(r'gpu busy ' + NUM + '%', t),
                    'load': grab(r'cpu load ' + NUM + '%', t), 'cores': grab(r'the game ' + NUM + r' cores', t),
                    'cpu_mhz': grab(r'cpu ' + NUM + r' MHz', t), 'gpu_mhz': grab(r'gpu ' + NUM + r' MHz', t)})
            elif t.startswith('sys: sensors'):
                info.append(t)
            elif t.startswith('fps: '):
                fps.append({'fps': grab(r'\(' + NUM + r' fps\)', t), 'speed': grab(r'speed ' + NUM, t)})
            elif t.startswith('mem: shadow at frame'):
                fr = grab(r'frame ([0-9]+)', t, int)
                n = fr - last_shadow_frame if last_shadow_frame is not None else None
                last_shadow_frame = fr
                if n and n > 0:
                    shadow.append({'frame': fr, 'ms': grab(r', ' + NUM + r' ms,', t) / n,
                                   'queries': grab(r', ([0-9]+) queries', t, int) / n,
                                   'in': grab(NUM + r' MiB copied in', t) / n, 'back': grab(NUM + r' MiB written back', t) / n,
                                   'pool_mib': grab(r'pools ' + NUM + r' MiB', t)})
            elif t.startswith('mem: write-watch frame'):
                ww.append({'frame': grab(r'frame ([0-9]+)', t, int), 'queries': grab(r'consumer: ' + NUM + r' queries', t),
                           'mib': grab(r'over ' + NUM + r' MiB', t), 'ms': grab(r'MiB, ' + NUM + r' ms', t),
                           'frozen': grab(r'frozen ' + NUM + r' MiB', t)})
            elif t.startswith('perf: frame') and 'interval avg' in t:
                perf.append({'frame': grab(r'frame ([0-9]+)', t, int), 'interval': grab(r'interval avg ' + NUM, t),
                             'max': grab(r' max ' + NUM + r' ms', t)})
            elif t.startswith('perf: frame') and 'gpu avg' in t:
                gpu.append({'frame': grab(r'frame ([0-9]+)', t, int), 'gpu': grab(r'gpu avg ' + NUM, t),
                            'vram': grab(r'vram ([0-9]+) of', t, int), 'ram': grab(r'working set ([0-9]+) MiB', t, int)})
            elif t.startswith('frametime: ') and 'frames in' in t:
                ft.append({'tag': t.split(':')[1].strip(), 'frames': grab(r': ([0-9]+) frames', t, int),
                           'secs': grab(r'frames in ' + NUM + r' s', t),
                           'mean': grab(r'mean ' + NUM + r' fps', t), 'low': grab(r'1% low ' + NUM, t),
                           'over50': grab(r'50 ms ([0-9]+)', t, int), 'over100': grab(r'100 ms ([0-9]+)', t, int)})
            elif t.startswith('audio: level'):
                audio.append(grab(r'rms ' + NUM, t))
            elif re.search(r'FATAL|hang: no present|device lost|DEVICE_LOST|validation error|not implemented|could not', t):
                bad.append(t[:200])
            elif t.startswith(('resolution: ', 'gfx: window ', 'vk: using gpu', 'mem: mode', 'oe: operation mode',
                               'gfx_shader: translator identity')):
                info.append(t[:160])
    if started is None:   # no focus line (a run without a window): the file's own time
        started = time.strftime('%Y-%m-%d %H:%M', time.localtime(os.path.getmtime(path)))
    return dict(sys=sysl, fps=fps, shadow=shadow, ww=ww, perf=perf, gpu=gpu, ft=ft, audio=audio, bad=bad, info=info,
                started=started)


def session_row(path):
    """One line per session for --list: start, minutes, frames, fps, APU and battery watts, peak temperatures, errors."""
    d = parse(path)
    s = d['sys']
    # without sys: lines (an older runtime) the frametime windows (60 s each, then the last one at exit) add up
    ft_frames = sum(r['frames'] or 0 for r in d['ft'])
    ft_secs = sum(r['secs'] or 0 for r in d['ft'])
    mins = (s[-1]['secs'] or 0) / 60 if s else (ft_secs / 60 if ft_secs else None)
    frames = s[-1]['frame'] if s else (ft_frames or None)

    def avg(k):
        v = [r[k] for r in s if r.get(k) is not None]
        return sum(v) / len(v) if v else None

    def peak(k):
        v = [r[k] for r in s if r.get(k) is not None]
        return max(v) if v else None
    fps = avg('fps')
    if fps is None and ft_secs:
        fps = ft_frames / ft_secs

    def f(v, fmt):
        return fmt.format(v) if v is not None else '-'
    return (f"{d['started']:16}  {f(mins, '{:5.1f}'):>5}  {f(frames, '{:7d}'):>7}  {f(fps, '{:5.1f}'):>5}  "
            f"{f(avg('apu'), '{:5.1f}'):>5}  {f(peak('apu_peak') or peak('apu'), '{:5.1f}'):>5}  "
            f"{f(avg('bat'), '{:5.1f}'):>5}  {f(peak('cpu_c'), '{:4.0f}'):>4}  {f(peak('gpu_c'), '{:4.0f}'):>4}  "
            f"{len(d['bad']):4d}  {os.path.basename(path)}")


def list_sessions(paths):
    print('start             min    frames    fps  APU W  peak W  bat W  CPU°C GPU°C errs file')
    for p in paths:
        try:
            print(session_row(p))
        except (OSError, EOFError) as e:
            print(f'(could not read {p}: {e})')


def stat(rows, key):
    v = [r[key] for r in rows if r.get(key) is not None]
    return (min(v), sum(v) / len(v), max(v)) if v else None


def show(title, rows, keys):
    out = []
    for key, label, unit in keys:
        s = stat(rows, key)
        if s:
            out.append(f'  {label}: mean {s[1]:.1f}{unit} (min {s[0]:.1f}, max {s[2]:.1f})')
    if out:
        print(title)
        print('\n'.join(out))


SYS_KEYS = [('fps', 'fps', ''), ('apu', 'APU', ' W'), ('bat', 'battery', ' W'), ('cpu_c', 'CPU', ' °C'),
            ('gpu_c', 'GPU', ' °C'), ('fan', 'fan', ' rpm'), ('gpu_busy', 'GPU busy', ' %'),
            ('load', 'CPU load', ' %'), ('cores', 'cores used by the game', ''), ('cpu_mhz', 'CPU clock', ' MHz'),
            ('gpu_mhz', 'GPU clock', ' MHz')]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('log', nargs='+')
    ap.add_argument('--from', dest='lo', type=int, default=0, help='first frame of the stretch')
    ap.add_argument('--to', dest='hi', type=int, default=1 << 62, help='last frame of the stretch')
    ap.add_argument('--rows', action='store_true', help='lists the sys: lines of the stretch')
    ap.add_argument('--list', action='store_true', help='one line per session (one log per session)')
    a = ap.parse_args()
    if a.list:
        list_sessions(a.log)
        return 0
    if len(a.log) != 1:
        ap.error('a single log (or --list for several)')
    a.log = a.log[0]
    d = parse(a.log)
    print(f'Session started at {d["started"]}')
    for t in d['info']:
        print('·', t)

    def cut(rows):
        return [r for r in rows if a.lo <= r.get('frame', a.lo) <= a.hi]
    rows = cut(d['sys'])
    span = f'frames {a.lo}–{a.hi}' if a.lo or a.hi < 1 << 62 else 'whole log'
    if rows:
        charging = any((r['bat_state'] or '').lower().startswith('charg') for r in rows)
        show(f'Machine ({span}; {len(rows)} sys: lines of ~10 s)', rows, SYS_KEYS)
        if charging:
            print('  (battery charging during part of the stretch: the battery figure is the charge, not the draw; use the APU one)')
        if not (a.lo or a.hi < 1 << 62) and len(rows) >= 6:   # the heaviest minute: six lines with the lowest fps
            worst = min(range(len(rows) - 5), key=lambda i: sum(r['fps'] or 0 for r in rows[i:i + 6]))
            w = rows[worst:worst + 6]
            show(f'Heaviest minute (frames {w[0]["frame"]}–{w[-1]["frame"]})', w, SYS_KEYS)
        if a.rows:
            print('  frame    fps   APU W  bat W  CPU°C GPU°C  GPU%  cores')
            for r in rows:
                def n(k, f='{:6.1f}'):
                    return f.format(r[k]) if r.get(k) is not None else '     -'
                print(f'  {r["frame"]:6d} {n("fps")} {n("apu")} {n("bat")} {n("cpu_c")} {n("gpu_c")} {n("gpu_busy")} {n("cores")}')
    else:
        print('(no sys: lines in this log: a runtime older than sysmon, or not Linux)')
    show('Frame (ACR_TRACE=perf)', cut(d['perf']), [('interval', 'interval', ' ms'), ('max', 'worst interval of the window', ' ms')])
    show('GPU (ACR_TRACE=perf)', cut(d['gpu']), [('gpu', 'GPU time per frame', ' ms'), ('vram', 'VRAM', ' MiB'), ('ram', 'RAM', ' MiB')])
    show('Pool shadows, per frame', cut(d['shadow']),
         [('ms', 'time', ' ms'), ('queries', 'queries', ''), ('in', 'copied to the shadow', ' MiB'),
          ('back', 'given back to the game', ' MiB')])
    show('Write watch, per frame', cut(d['ww']), [('ms', 'time', ' ms'), ('queries', 'queries', ''),
                                                   ('mib', 'memory queried', ' MiB'), ('frozen', 'frozen', ' MiB')])
    if d['fps'] and not (a.lo or a.hi < 1 << 62):
        show('fps: lines (120 frame windows)', d['fps'], [('fps', 'fps', ''), ('speed', 'game speed', '×')])
    for r in d['ft']:
        print(f'frametime {r["tag"]}: {r["frames"]} frames, mean {r["mean"]} fps, 1% low {r["low"]} fps, '
              f'over 50 ms {r["over50"]}, over 100 ms {r["over100"]}')
    if d['audio']:
        silent = sum(1 for x in d['audio'] if x == 0)
        print(f'Audio: {len(d["audio"])} windows of 5 s, {silent} fully silent, mean rms {sum(d["audio"]) / len(d["audio"]):.4f}')
    if d['bad']:
        print(f'Error or warning lines ({len(d["bad"])}):')
        for t in d['bad'][:12]:
            print('  !', t)
    return 0


if __name__ == '__main__':
    sys.exit(main())
