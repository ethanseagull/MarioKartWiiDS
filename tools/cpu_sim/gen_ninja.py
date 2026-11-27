# Writes build-<NAME>.ninja for the PC CPU simulator (tools/cpu_sim) of a source tree.
# Usage, from devkitPro's MSYS bash (g++ and ninja):
#   python tools/cpu_sim/gen_ninja.py TREE GENERATED_DIR NAME && ninja -f build-NAME.ninja
# TREE: the repository; GENERATED_DIR: a build's compile/generated (headers cooked from the disc,
# e.g. build/standard/compile/generated). Writes ./build-NAME/ objects and ./cpu_sim-NAME.exe.
import glob, os, sys

src, gen, name = sys.argv[1], sys.argv[2], sys.argv[3]
flags = ['-DREVOLUTION', '-DMKW_CPU_SIM', '-fcheck-new', '-ffp-contract=off', '-fno-asynchronous-unwind-tables',
         '-fno-exceptions', '-fno-rtti', '-fno-strict-aliasing', '-std=gnu++23', '-O2', '-w',
         '-I' + src + '/vendor/kinoko/include', '-I' + src + '/vendor/kinoko/source', '-I' + src + '/include', '-I' + gen]
out = ['ninja_required_version = 1.3', 'rule cc',
       '  command = g++ -MD -MT $out -MF $out.d %s -c $in -o $out' % ' '.join(flags),
       '  depfile = $out.d', '  deps = gcc', 'rule ld', '  command = g++ $in -o $out']
# Kinoko without its PC hosts (the simulator is the host).
skip = {'host/main.cc', 'host/KReplaySystem.cc', 'host/KTestSystem.cc'}
files = []
for f in glob.glob(src + '/vendor/kinoko/source/**/*.cc', recursive=True):
    rel = os.path.relpath(f, src + '/vendor/kinoko/source').replace(chr(92), '/')
    if rel not in skip:
        files.append(f.replace(chr(92), '/'))
# The game's race logic the simulator steps (src/main.cpp's race tick without drawing or sound).
GAME = ['cpu_ai', 'race_grid', 'race_items', 'item_native', 'item_world', 'shell_guidance', 'race_intro', 'intro_clearance']
files += [src + '/src/%s.cpp' % g for g in GAME]
files += sorted(f.replace(chr(92), '/') for f in glob.glob(src + '/tools/cpu_sim/*.cpp'))
objs = []
for f in files:
    o = 'build-%s/%s.o' % (name, os.path.relpath(f, src).replace(chr(92), '/').replace('../', ''))
    objs.append(o)
    out.append('build %s: cc %s' % (o, f))
out.append('build cpu_sim-%s.exe: ld %s' % (name, ' '.join(objs)))
open('build-%s.ninja' % name, 'w').write('\n'.join(out) + '\n')
print(len(objs), 'sources')
