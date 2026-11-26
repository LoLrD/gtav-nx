import concurrent.futures
import os
import re
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
build = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'build', 'mac')
out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(build, 'spirv')
tools = os.path.join(build, 'tools')
cache = os.path.join(build, 'shader_cache')
work = os.path.join(build, 'census_hlsl')


def c_string(text):
    return bytes(text, 'utf-8').decode('unicode_escape')


def variants():
    src = open(os.path.join(root, 'gtav', 'src', 'native', 'shader_compiler.cpp')).read()
    block = src[src.index('constexpr Variant kVariants[] = {'):]
    block = block[:block.index('};')]
    result = []
    for entry in re.findall(r'\{((?:\s*"(?:[^"\\]|\\.)*",?)+)\s*\}', block):
        result.append([c_string(s) for s in re.findall(r'"((?:[^"\\]|\\.)*)"', entry)])
    return result


VARIANTS = variants()


def dxc(hlsl, name, pixel):
    tmp = os.path.join(out_dir, name + '.spv.tmp')
    args = [os.path.join(tools, 'dxc'), '-T', 'ps_6_0' if pixel else 'vs_6_0', '-HV', '2021', '-all-resources-bound',
            '-spirv', '-fvk-use-dx-layout', '-Wno-ignored-attributes', '-Qstrip_debug']
    text = open(hlsl).read()
    if 'QuadReadAcross' in text:
        args.append('-fspv-target-env=vulkan1.1')
    if not pixel:
        args.append('-fvk-invert-y')
    args += ['-Fo', tmp, hlsl]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode:
        return r.stderr[-2000:]
    if not pixel and '= vfetchIndirect(' in text:
        open(os.path.join(out_dir, name + '.indirect'), 'w').close()
    os.replace(tmp, os.path.join(out_dir, name + '.spv'))
    return None


def one(name):
    pixel = name.startswith('ps_')
    hlsl = os.path.join(work, name + '.hlsl')
    r = subprocess.run([os.path.join(tools, 'XenosRecomp'), os.path.join(cache, name + '.bin'), hlsl,
                        os.path.join(tools, 'shader_common.h')], capture_output=True, text=True)
    if r.returncode or not os.path.exists(hlsl):
        return name, 'XenosRecomp: ' + r.stderr[-2000:]
    for v in VARIANTS:
        if v[0] != name:
            continue
        text = open(hlsl).read()
        for find, replace in zip(v[2::2], v[3::2]):
            if text.count(find) != 1:
                return name, 'variant ' + v[1] + ': line not found once'
            text = text.replace(find, replace)
        vh = os.path.join(work, name + v[1] + '.hlsl')
        open(vh, 'w').write(text)
        err = dxc(vh, name + v[1], pixel)
        if err:
            return name + v[1], err
    return name, dxc(hlsl, name, pixel)


def main():
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(work, exist_ok=True)
    names = sorted(f[:-4] for f in os.listdir(cache) if f.endswith('.bin') and f[:3] in ('ps_', 'vs_'))
    failed = []
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as ex:
        for i, (name, err) in enumerate(ex.map(one, names), 1):
            if err:
                failed.append(name)
                print(f'FAILED {name}: {err}', flush=True)
            if i % 500 == 0:
                print(f'{i} / {len(names)}', flush=True)
    print(f'{len(names) - len(failed)} of {len(names)} shaders compiled, {len(failed)} failed')
    sys.exit(1 if failed else 0)


main()
