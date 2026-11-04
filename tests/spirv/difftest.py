#!/usr/bin/env python3
"""Differential test of VkVita's SPIR-V to Cg translation. Run it with run.sh.

Each test is GLSL (name.frag, optional name.vert). The reference renders the
SPIR-V from glslang on lavapipe. The candidate translates the same SPIR-V to
Cg with spirv2cg, parses that Cg as HLSL with glslang, feeds the uniforms the
way VkVita does (from the reported table) and renders it the same way.

S2C_MODE picks the translator: -N (default) VkVita's own, -n with SPIRV-Cross
as fallback, empty for SPIRV-Cross. OPT=-O runs spirv-opt on the SPIR-V first.

Annotations in the GLSL:
  //@ubo <binding> <words...>   set 0 uniform buffer, words: 1.5 (float), i3 (int), u7 (uint)
  //@pc <words...>              push constants
  //@tex <binding> <pattern>    combined image sampler
  //@img <binding> <pattern>    sampled image
  //@smp <binding>              sampler
"""
import os, re, struct, subprocess, sys

S = os.path.dirname(os.path.abspath(__file__))
BUILD = os.environ.get('OUT', os.path.join(S, 'build'))
S2C = os.path.join(BUILD, 'spirv2cg')
RENDER = os.path.join(BUILD, 'render')
ENV = dict(os.environ, VK_ICD_FILENAMES='/usr/share/vulkan/icd.d/lvp_icd.json')
MODE = os.environ.get('S2C_MODE', '-N')

DEFAULT_VERT = """#version 450
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec2 vUv;
void main() { vUv = inUv; gl_Position = vec4(inPos, 0.0, 1.0); }
"""


def words(tokens):
    out = b''
    for t in tokens:
        if t.startswith('i'):
            out += struct.pack('<i', int(t[1:]))
        elif t.startswith('u'):
            out += struct.pack('<I', int(t[1:]))
        elif t == '_':
            out += b'\0\0\0\0'
        else:
            out += struct.pack('<f', float(t))
    return out


def annotations(src):
    ubo, pc, res = {}, b'', []
    for line in src.splitlines():
        m = re.match(r'\s*//@(\w+)\s*(.*)', line)
        if not m:
            continue
        kind, rest = m.group(1), m.group(2).split()
        if kind == 'ubo':
            ubo[int(rest[0])] = ubo.get(int(rest[0]), b'') + words(rest[1:])
        elif kind == 'pc':
            pc += words(rest)
        elif kind in ('tex', 'img'):
            res.append((kind, int(rest[0]), int(rest[1])))
        elif kind == 'smp':
            res.append((kind, int(rest[0]), 0))
    return ubo, pc, res


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def glsl_to_spv(path, out):
    r = run(['glslangValidator', '-V', '-o', out, path])
    if r.returncode:
        raise RuntimeError('glslang: ' + r.stdout + r.stderr)


TYPE_REGS = re.compile(r'(float|int|bool)(\d)?(?:x(\d))?$')


def regs_and_comps(t):
    m = TYPE_REGS.match(t)
    if not m:
        raise RuntimeError('type ' + t)
    a, b = m.group(2), m.group(3)
    if b:
        return int(a), int(b)   # rows, components per row
    return 1, int(a) if a else 1


def cg_to_hlsl(cg, stage, cbBinding, texSet):
    """Returns HLSL, the cbuffer layout [(name, reg, rows, comps, count)] and the textures [(unit, name)]."""
    lines = cg.splitlines()
    out = ['#pragma pack_matrix(row_major)']
    cb, texs, reg = [], [], 0
    sig = []
    for l in lines:
        m = re.match(r'uniform (sampler\w+) (\w+) : TEXUNIT(\d+);$', l)
        if m:
            texs.append((int(m.group(3)), m.group(2)))
            tt = {'sampler2D': 'Texture2D', 'samplerCUBE': 'TextureCube', 'sampler1D': 'Texture1D', 'sampler3D': 'Texture3D'}[m.group(1)]
            b = '[[vk::combinedImageSampler]][[vk::binding(%s, %d)]]' % (m.group(3), texSet)
            out.append('%s %s %s_t;\n%s SamplerState %s_s;' % (b, tt, m.group(2), b, m.group(2)))
            continue
        m = re.match(r'uniform (\w+) (\w+)(?:\[(\d+)\])?;$', l)
        if m:
            rows, comps = regs_and_comps(m.group(1))
            count = int(m.group(3)) if m.group(3) else 0
            cb.append((m.group(2), m.group(1), reg, rows, comps, count))
            reg += rows * max(count, 1)
            continue
        out.append(l)
    hl = rewrite_tex('\n'.join(out))

    def param(m):
        out_, ty, name, sem = m.group(1) or '', m.group(2), m.group(3), m.group(4)
        attr = ''
        if sem.startswith('ATTR'):
            attr = '[[vk::location(%s)]] ' % sem[4:]
        elif sem.startswith('TEXCOORD'):
            attr = '[[vk::location(%s)]] ' % sem[8:]
        elif sem == 'POSITION' or sem == 'WPOS':
            sem = 'SV_Position'
        elif sem == 'COLOR':
            sem = 'SV_Target0'
            attr = '[[vk::location(0)]] '
        elif sem == 'FACE':
            sem = 'SV_IsFrontFace'
            ty = 'bool'
        elif sem == 'INDEX':
            sem = 'SV_VertexID'
        elif sem == 'INSTANCE':
            sem = 'SV_InstanceID'
        elif sem == 'PSIZE':
            attr = '[[vk::builtin("PointSize")]] '
        return '%s%s%s %s : %s' % (attr, out_, ty, name, sem)

    hl = re.sub(r'(out )?(\w+) (stage_\w+) : (\w+)', param, hl)
    if cb:
        decl = ['[[vk::binding(%d, 1)]] cbuffer VKV%d' % (cbBinding, cbBinding), '{']
        for name, ty, r, rows, comps, count in cb:
            decl.append('    %s %s%s : packoffset(c%d);' % (ty, name, '[%d]' % count if count else '', r))
        decl.append('};')
        hl = hl.replace('#pragma pack_matrix(row_major)', '#pragma pack_matrix(row_major)\n' + '\n'.join(decl), 1)
    return hl, cb, texs


def split_args(s, start):
    """Arguments of the call whose '(' is at s[start]; returns (args, index after ')')."""
    depth, args, cur = 0, [], ''
    i = start
    while i < len(s):
        ch = s[i]
        if ch == '(':
            depth += 1
            if depth > 1:
                cur += ch
        elif ch == ')':
            depth -= 1
            if depth == 0:
                args.append(cur.strip())
                return args, i + 1
            cur += ch
        elif ch == ',' and depth == 1:
            args.append(cur.strip())
            cur = ''
        else:
            cur += ch
        i += 1
    raise RuntimeError('unbalanced call')


def rewrite_tex(src):
    pat = re.compile(r'\b(tex(?:1D|2D|3D|CUBE)(?:lod|bias|proj|grad)?)\(')
    while True:
        m = pat.search(src)
        if not m:
            return src
        fn = m.group(1)
        args, end = split_args(src, m.end() - 1)
        t, c = args[0] + '_t', args[1]
        sm = args[0] + '_s'
        if fn.endswith('lod'):
            n = 'xyz' if 'CUBE' in fn or '3D' in fn else 'xy'
            rep = '%s.SampleLevel(%s, (%s).%s, (%s).w)' % (t, sm, c, n, c)
        elif fn.endswith('bias'):
            n = 'xyz' if 'CUBE' in fn or '3D' in fn else 'xy'
            rep = '%s.SampleBias(%s, (%s).%s, (%s).w)' % (t, sm, c, n, c)
        elif fn.endswith('proj'):
            rep = '%s.Sample(%s, (%s).xy / (%s).z)' % (t, sm, c, c)
        elif fn.endswith('grad'):
            rep = '%s.SampleGrad(%s, %s, %s, %s)' % (t, sm, c, args[2], args[3])
        else:
            rep = '%s.Sample(%s, %s)' % (t, sm, c)
        src = src[:m.start()] + rep + src[end:]


def parse_table(cg):
    unis, texs = {}, {}
    for l in cg.splitlines():
        m = re.match(r'/\* uniform (\S+) <- (?:set (-?\d+) binding (\d+) \+(\d+)|push constants \+(\d+))(?: stride (\d+))?( int| uint)? \*/', l)
        if m:
            if m.group(5) is not None:
                key = ('pc', 0, int(m.group(5)))
            else:
                key = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
            unis[m.group(1)] = (key, int(m.group(6) or 0), (m.group(7) or '').strip())
        m = re.match(r'/\* TEXUNIT(\d+) <- (?:combined image sampler set (\d+) binding (\d+)\[(\d+)\]|image set (\d+) binding (\d+)\[(\d+)\])', l)
        if m:
            b = int(m.group(3) or m.group(6))
            texs[int(m.group(1))] = b
    return unis, texs


def fill_cbuffer(cb, unis, ubo, pc):
    size = 16 * max(1, sum(rows * max(count, 1) for _, _, _, rows, _, count in cb))
    buf = bytearray(size)
    for name, ty, reg, rows, comps, count in cb:
        if name not in unis:
            continue  # internal uniforms the test does not feed
        (key, stride, kind) = unis[name]
        src = pc if key[0] == 'pc' else ubo.get(key[1], b'')
        off = key[2]
        for k in range(max(count, 1)):
            estride = stride if stride else rows * 16
            for r in range(rows):
                for c in range(comps):
                    s = off + k * estride + (r * 16 if rows > 1 else 0) + c * 4
                    raw = src[s:s + 4] if s + 4 <= len(src) else b'\0\0\0\0'
                    if kind == 'int':
                        v = float(struct.unpack('<i', raw)[0])
                    elif kind == 'uint':
                        v = float(struct.unpack('<I', raw)[0])
                    else:
                        v = struct.unpack('<f', raw)[0]
                    d = (reg + k * rows + r) * 16 + c * 4
                    buf[d:d + 4] = struct.pack('<f', v)
    return bytes(buf)


def render(vspv, fspv, layout, tmp, tag):
    lp = os.path.join(tmp, tag + '.layout')
    with open(lp, 'w') as f:
        f.write('\n'.join(layout) + '\n')
    out = os.path.join(tmp, tag + '.raw')
    r = run([RENDER, vspv, fspv, lp, out], env=ENV)
    if r.returncode:
        raise RuntimeError('render %s: %s' % (tag, r.stderr))
    data = open(out, 'rb').read()
    return struct.unpack('<%df' % (len(data) // 4), data)


def test(fragPath):
    name = os.path.basename(fragPath)[:-5]
    tmp = os.path.join(BUILD, 'out', name)
    os.makedirs(tmp, exist_ok=True)
    vertPath = fragPath[:-5] + '.vert'
    if not os.path.exists(vertPath):
        vertPath = os.path.join(tmp, 'default.vert')
        open(vertPath, 'w').write(DEFAULT_VERT)
    srcs = open(vertPath).read() + open(fragPath).read()
    ubo, pc, res = annotations(srcs)
    files = {}
    for b, data in ubo.items():
        p = os.path.join(tmp, 'ubo%d.bin' % b)
        open(p, 'wb').write(data)
        files[b] = p
    pcp = os.path.join(tmp, 'pc.bin')
    open(pcp, 'wb').write(pc)

    ref = {}
    for st, p in (('vert', vertPath), ('frag', fragPath)):
        ref[st] = os.path.join(tmp, 'ref.' + st + '.spv')
        glsl_to_spv(p, ref[st])
    layout = ['ubo 0 %d %s' % (b, p) for b, p in files.items()]
    layout += ['%s 0 %d %d' % r if r[0] != 'smp' else 'smp 0 %d' % r[1] for r in res]
    if pc:
        layout.append('pc ' + pcp)
    a = render(ref['vert'], ref['frag'], layout, tmp, 'ref')

    cand = {}
    layout2 = []
    texPattern = {b: p for k, b, p in res if k in ('tex', 'img')}
    for i, st in enumerate(('vert', 'frag')):
        cgp = os.path.join(tmp, 'cand.' + st + '.cg')
        src = ref[st]
        if os.environ.get('OPT'):
            src = os.path.join(tmp, 'opt.' + st + '.spv')
            r = run(['spirv-opt', os.environ['OPT'], ref[st], '-o', src])
            if r.returncode:
                raise RuntimeError('spirv-opt: ' + r.stderr)
        args = [S2C] + ([MODE] if MODE else []) + ['-v' if st == 'vert' else '-f', src, cgp]
        r = run(args)
        if r.returncode:
            raise RuntimeError('translate %s: %s' % (st, r.stderr.strip()))
        cg = open(cgp).read()
        unis, texUnits = parse_table(cg)
        hl, cb, texs = cg_to_hlsl(cg, st, i, 2 + i)
        hp = os.path.join(tmp, 'cand.' + st + '.hlsl')
        open(hp, 'w').write(hl)
        cand[st] = os.path.join(tmp, 'cand.' + st + '.spv')
        r = run(['glslangValidator', '-V', '-D', '-S', st, '-e', 'main', '-o', cand[st], hp])
        if r.returncode:
            raise RuntimeError('hlsl %s: %s' % (st, (r.stdout + r.stderr).strip()[-800:]))
        if cb:
            cp = os.path.join(tmp, 'cb%d.bin' % i)
            open(cp, 'wb').write(fill_cbuffer(cb, unis, ubo, pc))
            layout2.append('ubo 1 %d %s' % (i, cp))
        for unit, _ in texs:
            layout2.append('tex %d %d %d' % (2 + i, unit, texPattern.get(texUnits.get(unit, -1), 0)))
    b = render(cand['vert'], cand['frag'], layout2, tmp, 'cand')
    diff = max(abs(x - y) for x, y in zip(a, b))
    return diff, a, b


def main():
    paths = sys.argv[1:] or sorted(
        os.path.join(S, 'shaders', f) for f in os.listdir(os.path.join(S, 'shaders')) if f.endswith('.frag'))
    bad = 0
    for p in paths:
        name = os.path.basename(p)
        try:
            diff, a, b = test(p)
        except RuntimeError as e:
            print('ERROR %-24s %s' % (name, str(e)[:600]))
            bad += 1
            continue
        ok = diff < 2e-3
        if not ok:
            bad += 1
        print('%s %-24s max diff %.5f  ref %s cand %s' % ('ok  ' if ok else 'FAIL', name, diff,
              ['%.3f' % v for v in a[:4]], ['%.3f' % v for v in b[:4]]))
    print('%d of %d failed' % (bad, len(paths)))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
