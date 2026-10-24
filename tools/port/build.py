"""Build and run the PC port: PS2Recomp's C++ of SLES_542.18 plus our game overrides in src/.

Everything it writes is under extracted/ps2recomp/ (gitignored); the submodule tools/PS2Recomp stays clean.

  python tools/port/build.py setup    worktree of tools/PS2Recomp at extracted/ps2recomp/tree, then the
                                           patch series tools/ps2recomp-patches/*.patch applied in order
  python tools/port/build.py export   Ghidra project -> extracted/ps2recomp/ghidra (RoRExportRecomp.java)
  python tools/port/build.py config   gen_config.py (functions.csv, config.toml, gen/ror_bindings.inc)
  python tools/port/build.py recomp   ps2_recomp -> output/, check_output.py, then sync
  python tools/port/build.py sync     copy changed generated files into the tree (setup does it after a reset)
  python tools/port/build.py build    CMake (configured on first use) -> ps2EntryRunner.exe
  python tools/port/build.py run [--frames N] [--seconds S] [--pad-script "FRAME+N:button,..."] [--env NAME=VALUE]
  python tools/port/build.py all      setup, config, recomp, build
"""
import argparse, ctypes, filecmp, glob, hashlib, os, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SUB = os.path.join(ROOT, 'tools', 'PS2Recomp')
PATCHES = os.path.join(ROOT, 'tools', 'ps2recomp-patches')
OUT = os.path.join(ROOT, 'extracted', 'ps2recomp')
TREE = os.path.join(OUT, 'tree')
RUNTIME_BUILD = os.path.join(TREE, 'out', 'build')
RECOMP_BUILD = os.path.join(TREE, 'out', 'recomp')
DISC = os.path.join(ROOT, 'extracted', 'disc')
GHIDRA = os.path.join(ROOT, 'ghidra')
JAVA_HOME = r'C:\Program Files\Java\jdk-26.0.1'
GENERATOR = os.environ.get('ROR_CMAKE_GENERATOR', 'Visual Studio 18 2026')  # e.g. "Visual Studio 17 2022"
# PS2Recomp's CMake fetches its dependencies (raylib, toml11, fmt, ELFIO, ...) into this cache, which is ignored
# inside the submodule. A fresh machine downloads them on the first configure; after that builds run offline.
DEPS = os.path.join(SUB, 'out', 'build', '_deps')
OFFLINE = all(os.path.isdir(os.path.join(DEPS, d)) for d in ('raylib-src', 'toml11-src', 'fmt-src', 'elfio-src'))
COMMON = ['-G', GENERATOR, '-DPS2X_BUILD_ANALYZER=OFF', '-DPS2X_BUILD_TEST=OFF', '-DPS2X_BUILD_STUDIO=OFF',
          '-DFETCHCONTENT_BASE_DIR=' + DEPS, '-DFETCHCONTENT_FULLY_DISCONNECTED=' + ('ON' if OFFLINE else 'OFF')]


def sh(cmd, **kw):
    print('>', ' '.join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def git(*a, cwd=SUB):
    return subprocess.run(['git', '-C', cwd] + list(a), check=True, capture_output=True, text=True).stdout.strip()


def setup():
    base = git('rev-parse', 'HEAD')
    if not os.path.isdir(TREE):
        sh(['git', '-C', SUB, 'worktree', 'add', '--detach', TREE, base])
    patches = sorted(glob.glob(os.path.join(PATCHES, '*.patch')))
    h = hashlib.sha256(base.encode())
    for p in patches:
        h.update(open(p, 'rb').read())
    stamp = os.path.join(OUT, 'tree.stamp')
    if os.path.exists(stamp) and open(stamp).read() == h.hexdigest():
        print('tree at %s with %d patches (unchanged)' % (base[:7], len(patches)))
        return
    # Files the reset and re-apply leave with the same content get their old timestamps back, so a new patch only
    # rebuilds what it changes (a touched runtime header would otherwise invalidate the precompiled header).
    before = {}
    for rel in git('diff', '--name-only', cwd=TREE).splitlines():
        path = os.path.join(TREE, rel)
        if os.path.isfile(path):
            before[path] = (hashlib.sha256(open(path, 'rb').read()).digest(), os.stat(path).st_mtime_ns)
    sh(['git', '-C', TREE, 'checkout', '-f', '--detach', base])  # resets tracked files only; builds stay
    for p in patches:
        sh(['git', '-C', TREE, 'apply', '--whitespace=nowarn', p])
    kept = 0
    for path, (digest, mtime) in before.items():
        if os.path.isfile(path) and hashlib.sha256(open(path, 'rb').read()).digest() == digest:
            os.utime(path, ns=(mtime, mtime))
            kept += 1
    open(stamp, 'w').write(h.hexdigest())
    print('tree at %s + %d patches (%d unchanged files keep their timestamps)' % (base[:7], len(patches), kept))
    # The checkout restored the tree's placeholder src/runner/register_functions.cpp: put the generated code back.
    if os.path.exists(os.path.join(OUT, 'output', 'register_functions.cpp')):
        sync()


def export():
    env = dict(os.environ, JAVA_HOME=os.environ.get('JAVA_HOME', JAVA_HOME))
    sh([os.path.join(GHIDRA, 'support', 'analyzeHeadless.bat'), os.path.join(GHIDRA, 'projects'), 'RuleOfRose',
        '-process', 'SLES_542.18', '-noanalysis', '-readOnly', '-scriptPath', os.path.join(ROOT, 'tools', 'ghidra_scripts'),
        '-postScript', 'RoRExportRecomp.java', os.path.join(OUT, 'ghidra')], env=env, stdout=subprocess.DEVNULL)


def config():
    sh([sys.executable, os.path.join(ROOT, 'tools', 'port', 'gen_config.py')])


def recomp_exe():
    exe = os.path.join(RECOMP_BUILD, 'ps2xRecomp', 'Release', 'ps2_recomp.exe')
    if not os.path.exists(os.path.join(RECOMP_BUILD, 'CMakeCache.txt')):
        sh(['cmake', '-S', TREE, '-B', RECOMP_BUILD, '-DPS2X_BUILD_RECOMP=ON', '-DPS2X_BUILD_RUNTIME=OFF'] + COMMON)
    sh(['cmake', '--build', RECOMP_BUILD, '--config', 'Release', '--target', 'ps2_recomp', '--', '/m', '/v:minimal'])
    return exe


def recomp():
    exe = recomp_exe()
    out = os.path.join(OUT, 'output')
    os.makedirs(out, exist_ok=True)
    for f in glob.glob(os.path.join(out, '*.cpp')) + glob.glob(os.path.join(out, '*.h')):
        os.remove(f)  # renamed functions must not leave stale files behind
    with open(os.path.join(OUT, 'recomp.log'), 'w') as log:
        sh([exe, os.path.join(OUT, 'config.toml')], cwd=OUT, stdout=log, stderr=subprocess.STDOUT)
    sh([sys.executable, os.path.join(ROOT, 'tools', 'port', 'check_output.py')])
    sync()


def sync():
    """Copy the generated C++ into the tree, only what changed, so an unchanged function keeps its timestamp and
    its unity batch is not rebuilt."""
    out = os.path.join(OUT, 'output')
    runner = os.path.join(TREE, 'ps2xRuntime', 'src', 'runner')
    include = os.path.join(TREE, 'ps2xRuntime', 'include')
    want = {os.path.basename(f) for f in glob.glob(os.path.join(out, '*.cpp'))}
    for f in glob.glob(os.path.join(runner, '*.cpp')):
        if os.path.basename(f) not in want:
            os.remove(f)
    copied = 0
    for name in want:
        copied += copy_if_changed(os.path.join(out, name), os.path.join(runner, name))
    for name in ('ps2_recompiled_functions.h', 'ps2_recompiled_stubs.h'):
        copied += copy_if_changed(os.path.join(out, name), os.path.join(include, name))
    print('synced: %d of %d files changed' % (copied, len(want) + 2))


def copy_if_changed(src, dst):
    if os.path.exists(dst) and filecmp.cmp(src, dst, shallow=False):
        return 0
    shutil.copy2(src, dst)
    return 1


def build():
    src = os.path.join(ROOT, 'src').replace('\\', '/')
    # Configure every time: when the set of generated files changes, a build that regenerates the project itself
    # keeps compiling with the old file list and leaves the new unity batches out (seen as LNK2019).
    t = time.time()
    sh(['cmake', '-S', TREE, '-B', RUNTIME_BUILD, '-DPS2X_BUILD_RECOMP=OFF', '-DPS2X_ENABLE_DEBUG_UI=OFF',
        '-DPS2X_ENABLE_FFMPEG=OFF', '-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF', '-DPS2X_GAME_SOURCE_DIR=' + src,
        '-DPS2X_GAME_INCLUDE_DIRS=' + os.path.join(OUT, 'gen').replace('\\', '/')] + COMMON, stdout=subprocess.DEVNULL)
    print('configure %.0f s' % (time.time() - t))
    t = time.time()
    sh(['cmake', '--build', RUNTIME_BUILD, '--config', 'Release', '--target', 'ps2EntryRunner', '--', '/m', '/v:minimal'])
    print('build %.0f s' % (time.time() - t))


def close_windows(pid):
    """WM_CLOSE to the process's top-level windows: the runner then stops the guest and flushes its output."""
    user32 = ctypes.windll.user32
    proc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def cb(hwnd, _):
        p = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
        return True
    user32.EnumWindows(proc(cb), 0)


def run(frames, seconds, pad_script='', extra_env=()):
    exe = os.path.join(RUNTIME_BUILD, 'ps2xRuntime', 'Release', 'ps2EntryRunner.exe')
    cwd = os.path.join(OUT, 'run')
    os.makedirs(cwd, exist_ok=True)
    env = dict(os.environ, ROR_LOG=os.path.join(cwd, 'ror.log'), ROR_DISC=DISC, ROR_MAX_FRAMES=str(frames),
               ROR_PAD_CONFIG=os.path.join(ROOT, 'config', 'pad.ini'), ROR_PAD_SCRIPT=pad_script)
    env.update(kv.split('=', 1) for kv in extra_env)
    t = time.time()
    with open(os.path.join(cwd, 'stdout.txt'), 'w') as o, open(os.path.join(cwd, 'stderr.txt'), 'w') as e:
        p = subprocess.Popen([exe, os.path.join(DISC, 'SLES_542.18')], cwd=cwd, env=env, stdout=o, stderr=e)
        try:
            p.wait(seconds)
        except subprocess.TimeoutExpired:
            print('timeout after %d s: closing the window' % seconds)
            close_windows(p.pid)
            try:
                p.wait(20)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
    print('exit %s after %.0f s; logs in %s (ror.log, stdout.txt, stderr.txt)' % (p.returncode, time.time() - t, cwd))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('step', choices=['setup', 'export', 'config', 'recomp', 'sync', 'build', 'run', 'all'])
    ap.add_argument('--frames', type=int, default=300, help='run: stop after this many frames (0 = never)')
    ap.add_argument('--seconds', type=int, default=120, help='run: close the window after this long')
    ap.add_argument('--pad-script', default='', help='run: ROR_PAD_SCRIPT, e.g. "100+4:cross"')
    ap.add_argument('--env', action='append', default=[], help='run: extra NAME=VALUE for the runner (repeatable)')
    args = ap.parse_args()
    if args.step == 'all':
        for f in (setup, config, recomp, build):
            f()
    elif args.step == 'run':
        run(args.frames, args.seconds, args.pad_script, args.env)
    else:
        globals()[args.step]()


if __name__ == '__main__':
    main()
