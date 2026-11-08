"""Build and run the PC port: PS2Recomp's C++ of SLES_542.18 plus our game overrides in src/.

Everything it writes is under extracted/ps2recomp/ (gitignored); the submodule tools/PS2Recomp stays clean.

  python tools/port/build.py setup    worktree of tools/PS2Recomp at extracted/ps2recomp/tree, then the
                                           patch series tools/ps2recomp-patches/*.patch applied in order; the same
                                           for tools/pcsx2 at extracted/pcsx2/tree with tools/pcsx2-patches
  python tools/port/build.py pcsx2-deps  PCSX2's Windows dependencies at the pinned tag's versions (its own
                                           build-dependencies.bat without Qt/ffmpeg) -> extracted/pcsx2/deps-<tag>
  python tools/port/build.py pcsx2    PCSX2 (tree + pinned deps) -> extracted/pcsx2/tree-build: pcsx2-gsrunner,
                                           the port's pcsx2-ror.dll (pcsx2-ror/) and its replay tool ror-gsreplay
                                           (run folder extracted/pcsx2/tree-run); build links and stages the DLL
  python tools/port/build.py ffmpeg   FFmpeg's avcodec/avutil DLLs for the movie player (src/ror_movie.cpp): the pinned
                                           gyan.dev 8.1.2 shared build, SHA-256 checked -> extracted/ffmpeg/bin;
                                           build stages them next to the runner (without them movies are skipped)
  python tools/port/build.py export   Ghidra project -> extracted/ps2recomp/ghidra (RoRExportRecomp.java)
  python tools/port/build.py config   gen_config.py (functions.csv, config.toml, gen/ror_bindings.inc)
  python tools/port/build.py recomp   ps2_recomp -> output/, check_output.py, then sync
  python tools/port/build.py sync     copy changed generated files into the tree (setup does it after a reset)
  python tools/port/build.py build    CMake (configured on first use) -> ps2EntryRunner.exe
  python tools/port/build.py run [--frames N] [--seconds S] [--pad-script "FRAME+N:button,..."] [--env NAME=VALUE]
  python tools/port/build.py all      setup, config, recomp, pcsx2, build
  python tools/port/build.py dist     the playtest package (after build): launcher/ (RuleOfRose.exe), the runner and
                                           its DLLs, the C++ runtime, pad.ini, gs.ini, port.ini, README, licences; no game
                                           files (the launcher extracts the tester's own ISO) -> extracted/dist/*.zip
"""
import argparse, ctypes, filecmp, glob, hashlib, os, re, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SUB = os.path.join(ROOT, 'tools', 'PS2Recomp')
PATCHES = os.path.join(ROOT, 'tools', 'ps2recomp-patches')
OUT = os.path.join(ROOT, 'extracted', 'ps2recomp')
TREE = os.path.join(OUT, 'tree')
RUNTIME_BUILD = os.path.join(TREE, 'out', 'build')
RECOMP_BUILD = os.path.join(TREE, 'out', 'recomp')
DISC = os.path.join(ROOT, 'extracted', 'disc')
# The movie player's MPEG-2 decoder (src/ror_movie.cpp loads avcodec/avutil at runtime). Not PCSX2's copy: PCSX2's
# Windows deps build FFmpeg without decoders (video capture only). gyan.dev's versioned release package; the SHA-256 is
# the one gyan.dev publishes next to it (packages/<name>.sha256).
FFMPEG_URL = 'https://www.gyan.dev/ffmpeg/builds/packages/ffmpeg-8.1.2-full_build-shared.7z'
FFMPEG_SHA256 = 'cba748035c21ce1431d0823c7a3a711f38616f89f87a265dceddf9b7f6749d2d'
FFMPEG_OUT = os.path.join(ROOT, 'extracted', 'ffmpeg')
FFMPEG_BIN = os.path.join(FFMPEG_OUT, 'bin')
PCSX2_SUB = os.path.join(ROOT, 'tools', 'pcsx2')
PCSX2_PATCHES = os.path.join(ROOT, 'tools', 'pcsx2-patches')
PCSX2_OUT = os.path.join(ROOT, 'extracted', 'pcsx2')
PCSX2_TREE = os.path.join(PCSX2_OUT, 'tree')
PCSX2_TAG = 'v2.8.2'
PCSX2_DEPS = os.path.join(PCSX2_OUT, 'deps-' + PCSX2_TAG)
PCSX2_BUILD = os.path.join(PCSX2_OUT, 'tree-build')
PCSX2_ROR_OUT = os.path.join(PCSX2_BUILD, 'extra', 'Release')   # pcsx2-ror.dll/.lib/.pdb
PCSX2_ROR_INCLUDE = os.path.join(ROOT, 'pcsx2-ror', 'include')
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
    if setup_tree(SUB, TREE, PATCHES, os.path.join(OUT, 'tree.stamp')):
        # The checkout restored the tree's placeholder src/runner/register_functions.cpp: put the generated code back.
        if os.path.exists(os.path.join(OUT, 'output', 'register_functions.cpp')):
            sync()
    setup_tree(PCSX2_SUB, PCSX2_TREE, PCSX2_PATCHES, os.path.join(PCSX2_OUT, 'tree.stamp'))


def setup_tree(sub, tree, patch_dir, stamp):
    """Worktree of the submodule's commit with the patch series applied; False when it was already up to date."""
    base = git('rev-parse', 'HEAD', cwd=sub)
    if not os.path.isdir(tree):
        sh(['git', '-C', sub, 'worktree', 'add', '--detach', tree, base])
    patches = sorted(glob.glob(os.path.join(patch_dir, '*.patch')))
    h = hashlib.sha256(base.encode())
    for p in patches:
        h.update(open(p, 'rb').read())
    if os.path.exists(stamp) and open(stamp).read() == h.hexdigest():
        print('%s at %s with %d patches (unchanged)' % (os.path.basename(sub), base[:7], len(patches)))
        return False
    # Files the reset and re-apply leave with the same content get their old timestamps back, so a new patch only
    # rebuilds what it changes (a touched runtime header would otherwise invalidate the precompiled header).
    before = {}
    for rel in git('diff', '--name-only', cwd=tree).splitlines():
        path = os.path.join(tree, rel)
        if os.path.isfile(path):
            before[path] = (hashlib.sha256(open(path, 'rb').read()).digest(), os.stat(path).st_mtime_ns)
    sh(['git', '-C', tree, 'checkout', '-f', '--detach', base])  # resets tracked files only; builds stay
    for p in patches:
        sh(['git', '-C', tree, 'apply', '--whitespace=nowarn', p])
    kept = 0
    for path, (digest, mtime) in before.items():
        if os.path.isfile(path) and hashlib.sha256(open(path, 'rb').read()).digest() == digest:
            os.utime(path, ns=(mtime, mtime))
            kept += 1
    open(stamp, 'w').write(h.hexdigest())
    print('%s at %s + %d patches (%d unchanged files keep their timestamps)' % (os.path.basename(sub), base[:7],
                                                                             len(patches), kept))
    return True


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
    # pcsx2-ror.dll (build.py pcsx2) is linked through its import library and delay-loaded: under ROR_GS=cpu the
    # runner never loads it (src/ror_pcsx2host_win32.cpp).
    ror_lib = os.path.join(PCSX2_ROR_OUT, 'pcsx2-ror.lib')
    if not os.path.exists(ror_lib):
        sys.exit('no %s: run build.py pcsx2 first' % ror_lib)
    t = time.time()
    sh(['cmake', '-S', TREE, '-B', RUNTIME_BUILD, '-DPS2X_BUILD_RECOMP=OFF', '-DPS2X_ENABLE_DEBUG_UI=OFF',
        '-DPS2X_ENABLE_FFMPEG=OFF', '-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF', '-DPS2X_GAME_SOURCE_DIR=' + src,
        '-DPS2X_GAME_INCLUDE_DIRS=' + ';'.join(p.replace('\\', '/') for p in (os.path.join(OUT, 'gen'), PCSX2_ROR_INCLUDE)),
        '-DPS2X_GAME_LINK_LIBRARIES=' + ror_lib.replace('\\', '/') + ';delayimp.lib',
        # /MAP: ps2EntryRunner.map, to resolve raw stack offsets (the watchdog's hang stacks, ROR_HANG_CDB)
        '-DPS2X_GAME_LINK_OPTIONS=/DELAYLOAD:pcsx2-ror.dll;/MAP'] + COMMON, stdout=subprocess.DEVNULL)
    print('configure %.0f s' % (time.time() - t))
    t = time.time()
    sh(['cmake', '--build', RUNTIME_BUILD, '--config', 'Release', '--target', 'ps2EntryRunner', '--', '/m', '/v:minimal'])
    stage_pcsx2(os.path.join(RUNTIME_BUILD, 'ps2xRuntime', 'Release'), ('pcsx2-ror.dll', 'pcsx2-ror.pdb'), PCSX2_ROR_OUT)
    dlls = glob.glob(os.path.join(FFMPEG_BIN, '*.dll'))
    for f in dlls:
        copy_if_changed(f, os.path.join(RUNTIME_BUILD, 'ps2xRuntime', 'Release', os.path.basename(f)))
    if not dlls:
        print('note: no FFmpeg DLLs (build.py ffmpeg): movies will be skipped')
    print('build %.0f s' % (time.time() - t))


def ffmpeg():
    """Download the pinned FFmpeg shared build, check its SHA-256, keep avcodec, avutil, swresample, the licence."""
    import urllib.request
    archive = os.path.join(FFMPEG_OUT, os.path.basename(FFMPEG_URL))
    os.makedirs(FFMPEG_OUT, exist_ok=True)
    if not os.path.exists(archive):
        print('downloading %s' % FFMPEG_URL)
        part = archive + '.part'
        with urllib.request.urlopen(FFMPEG_URL) as r, open(part, 'wb') as f:
            shutil.copyfileobj(r, f, 1 << 20)
        os.replace(part, archive)
    h = hashlib.sha256()
    with open(archive, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    if h.hexdigest() != FFMPEG_SHA256:
        os.replace(archive, archive + '.bad')
        sys.exit('%s: SHA-256 %s, expected %s (kept as .bad)' % (archive, h.hexdigest(), FFMPEG_SHA256))
    work = os.path.join(FFMPEG_OUT, 'x')
    shutil.rmtree(work, ignore_errors=True)
    sh([os.path.join(ROOT, 'tools', 'bin', '7zr.exe'), 'x', '-y', '-o' + work, archive], stdout=subprocess.DEVNULL)
    shutil.rmtree(FFMPEG_BIN, ignore_errors=True)
    os.makedirs(FFMPEG_BIN)
    kept = []
    # avcodec imports avutil and swresample (checked statically: PE import table)
    for f in glob.glob(os.path.join(work, '*', 'bin', '*.dll')) + glob.glob(os.path.join(work, '*', 'LICENSE*')):
        name = os.path.basename(f)
        if name.startswith(('avcodec-', 'avutil-', 'swresample-', 'LICENSE')):
            shutil.copy2(f, os.path.join(FFMPEG_BIN, name))
            kept.append(name)
    shutil.rmtree(work, ignore_errors=True)
    print('ffmpeg: %s -> %s' % (', '.join(sorted(kept)), FFMPEG_BIN))


DIST = os.path.join(ROOT, 'extracted', 'dist')
LAUNCHER_BUILD = os.path.join(ROOT, 'extracted', 'launcher-build')
VC_CRT = ('msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
# Never in a package: the game's own files (CLAUDE.md). The launcher extracts them from the tester's disc image.
GAME_FILE = re.compile(r'(^SLES_|^SCES_|^SLUS_|\.(iso|pss|rpk|ara|wrd|grb|irx|img|str|cnf|bin|xvab|tm2|i3d)$)', re.I)


def vc_crt_dir():
    """The newest installed x64 C++ runtime folder (VC/Redist: Microsoft's app-local deployment of the CRT)."""
    pf = os.environ.get('ProgramFiles', 'C:' + os.sep + 'Program Files')
    found = [d for d in glob.glob(os.path.join(pf, 'Microsoft Visual Studio', '*', '*', 'VC', 'Redist', 'MSVC', '*',
                                               'x64', 'Microsoft.VC*.CRT'))
             if all(os.path.exists(os.path.join(d, f)) for f in VC_CRT)]
    return sorted(found)[-1] if found else None


def dist():
    """Playtest package for a tester who owns the game (user's decision, 2026-11-08). The runner contains the game's
    code as recompiled C++, but no game files: RuleOfRose.exe (launcher/) checks the tester's PAL disc image and
    extracts it on the first run. GPL v3 parts (PS2Recomp's runtime, PCSX2 in pcsx2-ror.dll, FFmpeg) come with their
    licences and a source offer (NOTICE.txt)."""
    sh(['cmake', '-S', os.path.join(ROOT, 'launcher'), '-B', LAUNCHER_BUILD, '-G', GENERATOR], stdout=subprocess.DEVNULL)
    sh(['cmake', '--build', LAUNCHER_BUILD, '--config', 'Release', '--', '/m', '/v:minimal'])
    rel = os.path.join(RUNTIME_BUILD, 'ps2xRuntime', 'Release')
    crt = vc_crt_dir()
    if not crt:
        sys.exit('no Visual Studio x64 C++ runtime (VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT) found')
    rev = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(['git', 'status', '--porcelain', '--', 'src', 'launcher', 'pcsx2-ror', 'tools/port'],
                           cwd=ROOT, capture_output=True, text=True).stdout.strip()
    name = 'RuleOfRose-playtest-' + rev + ('-modified' if dirty else '')
    pkg = os.path.join(DIST, name)
    shutil.rmtree(pkg, ignore_errors=True)
    bin_dir = os.path.join(pkg, 'bin')
    os.makedirs(bin_dir)
    shutil.copy2(os.path.join(LAUNCHER_BUILD, 'Release', 'RuleOfRose.exe'), pkg)
    for f in ['ps2EntryRunner.exe', 'portable.txt'] + [os.path.basename(d) for d in glob.glob(os.path.join(rel, '*.dll'))]:
        shutil.copy2(os.path.join(rel, f), bin_dir)
    for d in ('resources', 'D3D12'):
        shutil.copytree(os.path.join(rel, d), os.path.join(bin_dir, d))
    for f in VC_CRT:
        shutil.copy2(os.path.join(crt, f), bin_dir)
    shutil.copy2(os.path.join(ROOT, 'config', 'pad.ini'), pkg)
    shutil.copy2(os.path.join(ROOT, 'launcher', 'gs.ini'), pkg)   # the testers' defaults: 2x, 4:3
    shutil.copy2(os.path.join(ROOT, 'launcher', 'README.txt'), pkg)
    shutil.copy2(os.path.join(ROOT, 'launcher', 'port.ini'), pkg)
    shutil.copy2(os.path.join(ROOT, 'launcher', 'NOTICE.txt'), pkg)
    lic = os.path.join(pkg, 'licenses')
    os.makedirs(lic)
    for src, dst in ((os.path.join(SUB, 'LICENSE'), 'PS2Recomp-GPLv3.txt'),
                     (os.path.join(ROOT, 'tools', 'pcsx2', 'COPYING.GPLv3'), 'PCSX2-GPLv3.txt'),
                     (os.path.join(FFMPEG_BIN, 'LICENSE'), 'FFmpeg-GPLv3.txt'),
                     (os.path.join(DEPS, 'raylib-src', 'LICENSE'), 'raylib-zlib.txt')):
        shutil.copy2(src, os.path.join(lic, dst))
    deps_lic = os.path.join(PCSX2_DEPS, 'licenses')
    if os.path.isdir(deps_lic):
        shutil.copytree(deps_lic, os.path.join(lic, 'pcsx2-deps'))
    bad = [os.path.relpath(os.path.join(d, f), pkg) for d, _, fs in os.walk(pkg) for f in fs if GAME_FILE.search(f)]
    if bad:
        shutil.rmtree(pkg, ignore_errors=True)
        sys.exit('refusing to package game-like files: %s' % ', '.join(bad[:10]))
    archive = shutil.make_archive(pkg, 'zip', DIST, name)
    print('dist: %s (%.0f MB); unpacked in %s' % (archive, os.path.getsize(archive) / 2**20, pkg))


def stage_pcsx2(dest, files, src_dir):
    """Next to an exe that uses PCSX2: `files` from src_dir, the pinned deps' DLLs, the D3D12 Agility SDK, PCSX2's
    resources/ and portable.txt (PCSX2 keeps its data there, not in Documents)."""
    os.makedirs(dest, exist_ok=True)
    n = 0
    for f in files:
        n += copy_if_changed(os.path.join(src_dir, f), os.path.join(dest, f))
    for f in glob.glob(os.path.join(PCSX2_DEPS, 'bin', '*.dll')):
        n += copy_if_changed(f, os.path.join(dest, os.path.basename(f)))
    for sub, dst in ((os.path.join(PCSX2_DEPS, 'bin', 'D3D12'), 'D3D12'),
                     (os.path.join(PCSX2_TREE, 'bin', 'resources'), 'resources')):
        for f in glob.glob(os.path.join(sub, '**', '*'), recursive=True):
            if os.path.isfile(f):
                target = os.path.join(dest, dst, os.path.relpath(f, sub))
                os.makedirs(os.path.dirname(target), exist_ok=True)
                n += copy_if_changed(f, target)
    open(os.path.join(dest, 'portable.txt'), 'a').close()
    print('staged PCSX2 into %s (%d files changed)' % (dest, n))


PCSX2_DEPS_SKIP = ('qtbase', 'qtimageformats', 'qtsvg', 'qttools', 'qttranslations', 'QtApng', 'ffmpeg', 'meson',
                   'pkgconf', 'amf-headers', 'libvpl', 'nv-codec-headers', 'opus', 'SVT-AV1', 'x264', 'KDDockWidgets')


def pcsx2_deps():
    """PCSX2's Windows dependencies at the pinned tag: the tree's build-dependencies.bat (same versions, same SHA-256
    checks) without Qt, KDDockWidgets and ffmpeg (the port has no Qt UI; ffmpeg is only video capture), and with
    Windows' tar in place of 7-Zip. Installs into extracted/pcsx2/deps-<tag>; takes ~20 minutes, once."""
    src = os.path.join(PCSX2_TREE, '.github', 'workflows', 'scripts', 'windows', 'build-dependencies.bat')
    work = os.path.join(PCSX2_OUT, 'deps-work')
    os.makedirs(work, exist_ok=True)
    lines = open(src, encoding='utf-8', newline='').read().replace('\r\n', '\n').split('\n')
    out, skip_block = [], False
    header = {'pushd %~dp0': 'set "SCRIPTDIR=%s"' % os.path.dirname(src),
              'set "SCRIPTDIR=%CD%"': 'cd /d "%s" || goto error' % work,
              r'cd ..\..\..\..': None, 'cd ..': None, 'mkdir deps': 'mkdir "%s"' % PCSX2_DEPS,
              'cd deps || goto error': None, 'set "INSTALLDIR=%CD%"': 'set "INSTALLDIR=%s"' % PCSX2_DEPS,
              'popd': None}
    for i, line in enumerate(lines):
        if i < 60 and line in header:
            if header[line] is not None:
                out.append(header[line])
            continue
        if line.startswith('call :downloadfile') and any(('"%s' % n) in line for n in PCSX2_DEPS_SKIP):
            continue
        if line == 'echo Building Qt base...':
            skip_block = True
        if line == 'echo "Building PlutoVG..."':
            skip_block = False
        if skip_block:
            continue
        if '%SEVENZIP%' in line:
            if '-apng.patch.gz' in line:   # a plain .gz: 7-Zip names the output after the gzip header
                line = ('py -c "import gzip,shutil;shutil.copyfileobj(gzip.open(\'lpng%LIBPNG%-apng.patch.gz\'),'
                        'open(\'libpng-%LIBPNGLONG%-apng.patch\',\'wb\'))" || goto error')
            elif '-o"agility-sdk-' in line:
                line = ('mkdir "agility-sdk-%AGILITYSDK%" && tar -xf "agility-sdk-%AGILITYSDK%.nupkg" '
                        '-C "agility-sdk-%AGILITYSDK%" || goto error')
            elif '"-x^!' in line:          # 7-Zip exclude switch -> tar --exclude
                excl, arc = re.match(r'\s*%SEVENZIP% x "-x\^!([^"]+)" "([^"]+)"', line).groups()
                line = 'tar -xf "%s" --exclude "%s" || goto error' % (arc, excl.replace('\\', '/'))
            else:
                line = re.sub(r'%SEVENZIP% x ', 'tar -xf ', line)
        # Windows' bsdtar reads .zip too; Git's GNU tar (first on PATH under Git Bash) does not.
        out.append(line.replace('tar -xf ', '"%SystemRoot%\\System32\\tar.exe" -xf '))
    script = os.path.join(work, 'build-dependencies-ror.bat')
    open(script, 'w', newline='\r\n').write('\n'.join(out))
    t = time.time()
    sh(['cmd', '/c', script], cwd=work)
    print('pcsx2-deps %.0f s -> %s' % (time.time() - t, PCSX2_DEPS))


def pcsx2():
    """PCSX2 from the patched tree with the pinned deps (as step 0: no Qt UI, no tests, the gsrunner on) plus the
    port's pcsx2-ror DLL (pcsx2-ror/, PCSX2_EXTRA_PROJECT_DIR from tools/pcsx2-patches/0003) and its replay client
    ror-gsreplay; the gsrunner and ror-gsreplay get a run folder extracted/pcsx2/tree-run."""
    if not os.path.isdir(os.path.join(PCSX2_DEPS, 'bin')):
        sys.exit('no %s: run build.py pcsx2-deps first' % PCSX2_DEPS)
    t = time.time()
    sh(['cmake', '-S', PCSX2_TREE, '-B', PCSX2_BUILD, '-G', GENERATOR, '-A', 'x64',
        '-DCMAKE_PREFIX_PATH=' + PCSX2_DEPS.replace('\\', '/'), '-DENABLE_QT_UI=OFF', '-DENABLE_TESTS=OFF',
        '-DENABLE_GSRUNNER=ON', '-DDISABLE_ADVANCE_SIMD=ON',
        '-DPCSX2_EXTRA_PROJECT_DIR=' + os.path.join(ROOT, 'pcsx2-ror').replace('\\', '/')], stdout=subprocess.DEVNULL)
    sh(['cmake', '--build', PCSX2_BUILD, '--config', 'Release', '--target', 'pcsx2-gsrunner', 'pcsx2-ror',
        'ror-gsreplay', '--', '-m', '-v:minimal'])
    run = os.path.join(PCSX2_OUT, 'tree-run')
    stage_pcsx2(run, ('pcsx2-gsrunner.exe', 'pcsx2-gsrunner.pdb'), os.path.join(PCSX2_BUILD, 'pcsx2-gsrunner', 'Release'))
    # The replay tool (gs_regress.py) with the DLL it is a client of; the same resources/ and deps as the gsrunner's.
    stage_pcsx2(run, ('ror-gsreplay.exe', 'ror-gsreplay.pdb', 'pcsx2-ror.dll', 'pcsx2-ror.pdb'), PCSX2_ROR_OUT)
    print('pcsx2 %.0f s' % (time.time() - t))


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
               ROR_PAD_CONFIG=os.path.join(ROOT, 'config', 'pad.ini'), ROR_PAD_SCRIPT=pad_script,
               ROR_GS_INI=os.path.join(ROOT, 'config', 'gs.ini'))
    env.update(kv.split('=', 1) for kv in extra_env)
    cdb = os.path.join(os.environ.get('ProgramFiles(x86)', 'C:' + os.sep + 'Program Files (x86)'), 'Windows Kits', '10',
                       'Debuggers', 'x64', 'cdb.exe')
    if os.path.exists(cdb):
        env.setdefault('ROR_HANG_CDB', cdb)   # the watchdog's host stacks if the shutdown hangs (src/ror_main.cpp)
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
    if p.returncode == 3:
        print('note: the game thread did not stop after the last frame (shutdown hang); the watchdog lines in '
              'ror.log say where the guest was')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('step', choices=['setup', 'export', 'config', 'recomp', 'sync', 'build', 'run', 'all',
                                     'pcsx2-deps', 'pcsx2', 'ffmpeg', 'dist'])
    ap.add_argument('--frames', type=int, default=300, help='run: stop after this many frames (0 = never)')
    ap.add_argument('--seconds', type=int, default=120, help='run: close the window after this long')
    ap.add_argument('--pad-script', default='', help='run: ROR_PAD_SCRIPT, e.g. "100+4:cross"')
    ap.add_argument('--env', action='append', default=[], help='run: extra NAME=VALUE for the runner (repeatable)')
    args = ap.parse_args()
    if args.step == 'all':
        for f in (setup, config, recomp, pcsx2, build):
            f()
    elif args.step == 'run':
        run(args.frames, args.seconds, args.pad_script, args.env)
    else:
        globals()[args.step.replace('-', '_')]()


if __name__ == '__main__':
    main()
