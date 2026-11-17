"""Cataclysm's model shaders (4.3.4) for the 3.3.5 engine: read from Cata's own `.bls` files, repacked
into 3.3.5's container and permutation order, so the engine loads them like any stock shader.

Only what 3.3.5 lacks is taken: the vertex and pixel shaders Cata's packed model shader table names
(Engine/Source/Model2/M2Shader.cpp there) that the client has no file for. A name the client has
keeps the client's own shader.

Cata's container (`GXSH` version 0x10004, CGxShader::Load / CGxShaderPermute::Load in the Cata
decomp): `uint32 magic, version, permuteOffsetsOffset, permuteCount, blockOffsetsOffset, blockCount,
blockDataOffset`; the permutations' bytes are cut from one stream of 16 KiB blocks, each zlib-compressed
unless its stored size is 0x4000. A permutation is a 28-byte header, `uint32 size` and the DX9 token
stream.

The two clients' model shaders read the same constants (projection c2..c5, texture matrices c6..c9, sun
c10..c12, lights c17..c27, colour c28/c29, fog c30, bones from c31; pixel fog colour c2). Cata adds a
fog start in c30.w, which 3.3.5 always sets to 0, and the pixel row c4: the texture weights of the
`_Wgt` and crossfade shaders (CM2SceneRender::SetupTextures sets it for retail batches). The
permutations are ordered differently:

  3.3.5 vertex (90): local + 2 * (lights + 5 * (bones + 3 * shadow)), lights 0..4, shadow 0..2
  Cata vertex  (96): flag + 4 * (lightSet + 4 * (bones + 3 * shadow)), lightSet 0 unlit, 1 sun,
                     2 one light, 3 four lights; shadow 0..1; the flag picks nothing in these files
  pixel (16, both):  shadow + 4 * (pcf + 2 * alphaTest)

So a 3.3.5 permutation takes Cata's of the same bones and lighting; two or three lights take the
four-light permutation (3.3.5 zeroes the unused lights' colours and gives them a neutral attenuation,
CShaderEffect::ComputeLocalLights). The shadow permutations read other constants in the two clients:
batches drawn with Cata's shaders get the unshadowed permutation (no stock shadow on them).
"""
import hashlib
import os
import struct
import zlib

from . import dx9ir as ir

# Cata's packed model shader table (s_modelVertexShaders / s_modelPixelShaders as s_shaderEffects
# uses them), the names this module may take from Cata.
MODEL_VERTEX = ('Diffuse_T1_Env', 'Diffuse_T1_Env_T1', 'Diffuse_T1_T1', 'Diffuse_T1_T2', 'Diffuse_T1_T1_T1',
                'Diffuse_EdgeFade_T1', 'Diffuse_T1_Env_T2', 'Diffuse_EdgeFade_T1_T2', 'Diffuse_T1_T1_T1_T2',
                'Diffuse_EdgeFade_Env', 'Diffuse_T1_T2_T1')
MODEL_PIXEL = ('Combiners_Opaque_Mod2xNA_Alpha', 'Combiners_Opaque_AddAlpha', 'Combiners_Opaque_AddAlpha_Alpha',
               'Combiners_Opaque_Mod2xNA_Alpha_Add', 'Combiners_Mod_AddAlpha', 'Combiners_Mod_AddAlpha_Alpha',
               'Combiners_Opaque_Alpha_Alpha', 'Combiners_Opaque_Mod2xNA_Alpha_3s', 'Combiners_Opaque_AddAlpha_Wgt',
               'Combiners_Mod_Add_Alpha', 'Combiners_Opaque_ModNA_Alpha', 'Combiners_Mod_AddAlpha_Wgt',
               'Combiners_Opaque_Mod_Add_Wgt', 'Combiners_Opaque_Mod2xNA_Alpha_UnshAlpha', 'Combiners_Mod_Dual_Crossfade',
               'Combiners_Mod', 'Combiners_Opaque_Mod2xNA_Alpha_Alpha', 'Combiners_Mod_Mod',
               'Combiners_Mod_Masked_Dual_Crossfade', 'Combiners_Opaque_Alpha', 'Guild', 'Guild_NoBorder', 'Guild_Opaque')

PROFILE = {'vertex': 'vs_3_0', 'pixel': 'ps_3_0'}
PERMUTATIONS = {'vertex': 90, 'pixel': 16}   # what 3.3.5's CShaderEffect loads per shader
BS = chr(92)


def read_container(data):
    """The DX9 token streams of a Cata (0x10004) container, by permutation."""
    magic, version, poff, pcount, boff, bcount, doff = struct.unpack_from('<7I', data, 0)
    if magic != 0x47585348 or version != 0x10004:
        raise ValueError('not a Cataclysm shader container (version 0x%x)' % version)
    starts = struct.unpack_from('<%dI' % (pcount + 1), data, poff)
    blocks = struct.unpack_from('<%dI' % (bcount + 1), data, boff)
    stream = b''.join(
        data[doff + blocks[i]:doff + blocks[i + 1]] if blocks[i + 1] - blocks[i] == 0x4000
        else zlib.decompress(data[doff + blocks[i]:doff + blocks[i + 1]])
        for i in range(bcount))
    out = []
    for i in range(pcount):
        rec = stream[starts[i]:starts[i + 1]]
        size = struct.unpack_from('<I', rec, 28)[0]
        out.append((struct.unpack_from('<2I', rec, 0), rec[32:32 + size]))
    return out


def vertex_order(p):
    """The Cata permutation that stands for 3.3.5 vertex permutation `p`."""
    local, lights, rest = p % 2, (p // 2) % 5, p // 10
    bones = rest % 3
    light_set = 0 if not local else min(lights, 2) + 1
    return 4 * (light_set + 4 * bones)


def pixel_order(p):
    """The Cata permutation that stands for 3.3.5 pixel permutation `p` (unshadowed)."""
    return p & ~3


def container(records):
    """A 3.3.5 (0x10003) container of `records` [(a, b, code)]."""
    out = [struct.pack('<III', 0x47585348, 0x10003, len(records))]
    for a, b, code in records:
        out.append(struct.pack('<IIHHI', a, b, 0, 0, len(code)))
        out.append(code)
        out.append(b'\0' * (((len(code) + 3) & ~3) - len(code)))
    return b''.join(out)


class Repacked(object):
    """One Cata shader in 3.3.5's order: the container bytes and its records."""

    def __init__(self, kind, name, cata):
        order = vertex_order if kind == 'vertex' else pixel_order
        self.kind, self.name = kind, name
        self.path = BS.join(['Shaders', kind.capitalize(), PROFILE[kind], name + '.bls'])
        self.records = [cata[order(p)] for p in range(PERMUTATIONS[kind])]
        self.data = container([(hdr[0], hdr[1], code) for hdr, code in self.records])


def repack(cata_dir, have):
    """Every model shader of Cata's table the client lacks, from Cata's shader folder (the one holding
    Vertex\\ and Pixel\\). `have(kind, name)` says whether the client has the shader already."""
    out = []
    for kind, names in (('vertex', MODEL_VERTEX), ('pixel', MODEL_PIXEL)):
        for name in names:
            if have(kind, name):
                continue
            path = os.path.join(cata_dir, kind.capitalize(), PROFILE[kind], name + '.bls')
            if not os.path.isfile(path):
                raise SystemExit('no %s in the Cataclysm shaders (%s)' % (name, path))
            with open(path, 'rb') as f:
                out.append(Repacked(kind, name, read_container(f.read())))
    return out


def records(repacked, record_class):
    """The archive.Record list of the repacked shaders, as archive.enumerate_records makes them."""
    out = []
    for rp in repacked:
        for i, (hdr, code) in enumerate(rp.records):
            r = record_class()
            r.kind, r.profile, r.name, r.perm = rp.kind, PROFILE[rp.kind], rp.name, i
            r.code = code
            r.md5 = hashlib.md5(code).hexdigest()
            r.hdr = (hdr[0], hdr[1], 0, 0)
            r.sh = ir.decode(code)
            out.append(r)
    return out
