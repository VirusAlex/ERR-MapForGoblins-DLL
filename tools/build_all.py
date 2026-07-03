#!/usr/bin/env python3
"""Parallel multi-profile build orchestrator + profiler.

src/generated_shared/* (logo, map/overlay icon art, baked i18n strings) is shared
and profile-INDEPENDENT, so it is generated ONCE up front. Each profile's build
then runs with build.bat --skip-shared, so the per-profile work (data pipeline +
DLL compile - both isolated per profile: own data/<p> cache, own generated_<p>,
own builds/build-<p>) runs concurrently without racing on src/generated_shared.

LTCG linking is RAM-heavy, so concurrency is capped by --jobs (default 4). Each
build.bat still uses msbuild /m internally, so don't set --jobs too high.

Usage:
  py tools/build_all.py [snapshot|release] [--force-all] [--scan] [--jobs N] [--profiles a,b,c] [--skip-aob-check]

--scan VT-scans each profile's DLL the moment ITS build finishes (via
dashboard/vt_runner.py), so scans overlap with the still-running builds instead
of waiting for all eight; a per-profile MS-verdict summary prints at the end.

Prints a profiling summary: gen_shared per-script, per-profile pipeline vs compile,
slowest pipeline stages, and wall-clock vs sequential-sum (the parallel saving).
"""
import sys, os, time, re, subprocess, concurrent.futures
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / 'tools'
LOG = ROOT / 'scratch'
LOG.mkdir(exist_ok=True)
# convergence2 (Convergence 2.x / ME2) is unpublished/unsupported - excluded from the default
# all-profiles build. Build it explicitly with --profiles convergence2 if ever needed.
ALL = ['vanilla', 'err', 'convergence3', 'erte', 'goldenage', 'vins', 'reborn', 'graceborne']
GEN_SHARED = ['generate_logo.py', 'generate_map_icons.py', 'generate_overlay_icons.py', 'generate_i18n.py']


def run_to_log(cmd, logpath):
    with open(logpath, 'w', encoding='utf-8', errors='replace') as f:
        return subprocess.run(cmd, cwd=str(ROOT), stdout=f, stderr=subprocess.STDOUT).returncode


def parse_log(logpath):
    """Extract pipeline time, per-stage ran-times, and msbuild time from a build log."""
    txt = Path(logpath).read_text(encoding='utf-8', errors='replace')
    info = {'pipeline': None, 'msbuild': None, 'stages': [], 'success': False}
    m = re.search(r'\[DONE\]\s+pipeline in ([\d.]+)s', txt)
    if m:
        info['pipeline'] = float(m.group(1))
    for sm in re.finditer(r'\[OK\]\s+(\S+)\s+\(([\d.]+)s\)', txt):
        info['stages'].append((sm.group(1), float(sm.group(2))))
    # msbuild "Time Elapsed 00:00:NN.nn"
    m = re.search(r'Time Elapsed\s+(\d+):(\d+):([\d.]+)', txt)
    if m:
        info['msbuild'] = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + float(m.group(3))
    # snapshot prints "[SUCCESS] Snapshot packaged"; release prints "Release packaged".
    info['success'] = ('[SUCCESS]' in txt) or ('Release packaged' in txt)
    return info


def main():
    args = sys.argv[1:]
    mode = 'release' if 'release' in args else 'snapshot'
    force_all = '--force-all' in args
    scan = '--scan' in args   # VT-scan each DLL the moment its build finishes (overlaps scans with builds)
    skip_aob = '--skip-aob-check' in args  # skip the one-time exe AOB signature check (offline/no-exe)
    jobs = 4  # Ryzen 5800X 8C/16T + 32GB: LTCG is RAM-light here, CPU-bound; 4-6 is the sweet spot
    profiles = ALL
    for i, a in enumerate(args):
        if a == '--jobs' and i + 1 < len(args):
            jobs = int(args[i + 1])
        elif a.startswith('--jobs='):
            jobs = int(a.split('=', 1)[1])
        elif a == '--profiles' and i + 1 < len(args):
            profiles = [p.strip() for p in args[i + 1].split(',') if p.strip()]
        elif a.startswith('--profiles='):
            profiles = [p.strip() for p in a.split('=', 1)[1].split(',') if p.strip()]

    print(f'=== build_all: mode={mode} force_all={force_all} scan={scan} jobs={jobs} '
          f'profiles={",".join(profiles)} ===')
    wall0 = time.time()

    # ---- AOB signature check ONCE (exe-independent of profile: one vanilla exe
    # backs every profile). Per-profile build.bat runs are passed --skip-aob-check
    # so this doesn't repeat N times. A critical miss aborts before any build.
    if not skip_aob:
        print('\n[0] eldenring.exe AOB signature check (once)')
        rc = subprocess.run([sys.executable, str(TOOLS / 'check_aobs.py')], cwd=str(ROOT)).returncode
        if rc != 0:
            print('AOB signature check FAILED - aborting build_all. '
                  '(Pass --skip-aob-check to build without the game exe.)')
            sys.exit(1)

    # Version (for the VT scan rows) read straight from CMakeLists - no bump happened here.
    vt_ver = ''
    if scan:
        m = re.search(r'\n\s*VERSION\s+"([^"]+)"', (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8'))
        vt_ver = m.group(1) if m else ''
        VT_RUNNER = TOOLS / 'dashboard' / 'vt_runner.py'

    # ---- Phase 1: gen_shared ONCE ----
    print('\n[1] gen_shared (once, shared)')
    gen_times = {}
    for s in GEN_SHARED:
        t0 = time.time()
        rc = run_to_log([sys.executable, str(TOOLS / s)], LOG / f'ba_gen_{s}.log')
        gen_times[s] = time.time() - t0
        print(f'    {s:30} {gen_times[s]:6.1f}s  {"OK" if rc == 0 else f"FAIL({rc})"}')
        if rc != 0:
            print(f'    gen_shared FAILED ({s}); see scratch/ba_gen_{s}.log'); sys.exit(1)
    gen_total = time.time() - wall0

    # ---- Phase 2: per-profile builds in parallel (--skip-shared) ----
    print(f'\n[2] builds: {len(profiles)} profiles x {jobs} workers (--skip-shared)')

    def build_one(p):
        flag = [] if p == 'err' else [f'--{p}']
        # In release mode build.bat bumps the CMake version as its last step; with
        # parallel profiles that would bump N times AND race on CMakeLists. So pass
        # --no-bump to ALL profiles here and bump ONCE manually after build_all
        # (the /release skill Step 6 fallback). Snapshot mode never bumps.
        no_bump = ['--no-bump'] if mode == 'release' else []
        # --skip-aob-check: the exe check already ran once above (phase [0]).
        cmd = ['cmd', '/c', str(ROOT / 'build.bat'), mode, '--skip-shared', '--skip-aob-check'] \
            + (['--force-all'] if force_all else []) + no_bump + flag
        logp = LOG / f'ba_{p}.log'
        t0 = time.time()
        rc = run_to_log(cmd, logp)
        return p, time.time() - t0, rc, logp

    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = {ex.submit(build_one, p): p for p in profiles}
        for fut in concurrent.futures.as_completed(futs):
            p, dt, rc, logp = fut.result()
            results[p] = (dt, rc, parse_log(logp))
            print(f'    {p:14} {dt:6.1f}s  {"OK" if rc == 0 else f"FAIL({rc})"}')

    # ---- Phase 2b: VirusTotal scan (ONE process, sequential + rate-limited) ----
    # Gate on the PARSED "[SUCCESS]" flag, NOT build.bat's exit code: invoking build.bat
    # via `cmd /c` can surface a spurious non-zero rc even on a packaged build, which
    # previously skipped the scan of good DLLs. And run ONE vt_runner for all built
    # profiles (not one process per profile) so a single sqlite connection + the
    # in-process VT rate-limiter apply - concurrent per-profile scans used to race on
    # the DB (lost rows) and blow VirusTotal's request budget (timeouts).
    if scan:
        built = [p for p in profiles if results[p][2]['success']]
        if built:
            print(f'\n[2b] VirusTotal: scanning {len(built)} built profile(s) sequentially...')
            vlog = LOG / 'ba_vt_all.log'
            run_to_log([sys.executable, str(VT_RUNNER), '--profiles', ','.join(built),
                        '--version', vt_ver, '--change', f'v{vt_ver} release'], vlog)
            try:
                for ln in vlog.read_text(encoding='utf-8', errors='replace').splitlines():
                    if any(t in ln for t in (': done', ': cached', ': timeout', ': ERROR', ': missing')):
                        print('    ' + ln.strip())
            except Exception:
                pass
        else:
            print('\n[2b] VirusTotal: no successfully-built DLLs to scan')

    wall = time.time() - wall0

    # ---- Phase 3: profiling summary ----
    print('\n=== PROFILE ===')
    print(f'gen_shared total: {gen_total:.1f}s  ' +
          '  '.join(f'{s.replace("generate_","").replace(".py","")}={t:.0f}s'
                    for s, t in gen_times.items()))
    print(f'\n{"profile":14} {"total":>7} {"pipeline":>9} {"compile":>8}  status')
    seq_sum = gen_total
    for p in profiles:
        dt, rc, info = results[p]
        seq_sum += dt
        pipe = info['pipeline']
        comp = info['msbuild']
        compe = comp if comp is not None else (dt - pipe if pipe is not None else None)
        print(f'{p:14} {dt:6.1f}s {("%.1fs"%pipe) if pipe is not None else "  -  ":>9} '
              f'{("%.1fs"%compe) if compe is not None else "  -  ":>8}  '
              f'{"OK" if rc == 0 and info["success"] else "FAIL"}')

    # slowest pipeline stages across all profiles (useful for --force-all)
    agg = {}
    for p in profiles:
        for name, t in results[p][2]['stages']:
            agg[name] = agg.get(name, 0.0) + t
    if agg:
        print('\nslowest pipeline stages (summed across profiles that RAN them):')
        for name, t in sorted(agg.items(), key=lambda x: -x[1])[:10]:
            print(f'    {name:34} {t:6.1f}s')

    print(f'\nwall-clock: {wall:.1f}s   |   sequential-sum: {seq_sum:.1f}s   |   '
          f'parallel saved ~{seq_sum - wall:.1f}s')
    if any(r[1] != 0 or not r[2]['success'] for r in results.values()):
        print('\nWARNING: some builds FAILED - check scratch/ba_<profile>.log')
        sys.exit(1)


if __name__ == '__main__':
    main()
