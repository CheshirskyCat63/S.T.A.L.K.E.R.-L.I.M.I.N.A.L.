"""Strict OGF v4 skeletal reader/writer. Unknown payloads remain byte-for-byte intact."""
from __future__ import annotations
import hashlib
import math
import struct
from pathlib import Path


class ResourceError(ValueError):
    def __init__(self, code, detail):
        self.code, self.detail = code, detail
        super().__init__(f"{code}: {detail}")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def chunks(data):
    result, offset = [], 0
    while offset < len(data):
        if offset + 8 > len(data):
            raise ResourceError('OGF_TRUNCATED', {'offset': offset})
        kind, size = struct.unpack_from('<II', data, offset)
        offset += 8
        if kind & 0x80000000 or offset + size > len(data):
            raise ResourceError('OGF_UNSUPPORTED_CHUNK', {'kind': kind, 'offset': offset, 'size': size})
        result.append((kind, data[offset:offset + size]))
        offset += size
    if len({key for key, _ in result}) != len(result):
        raise ResourceError('OGF_DUPLICATE_CHUNK', {})
    return result


def encode_chunks(items):
    return b''.join(struct.pack('<II', key, len(data)) + data for key, data in items)


def zstring(data, offset):
    end = data.index(0, offset)
    return data[offset:end].decode('cp1251'), end + 1


def add(a, b): return tuple(x + y for x, y in zip(a, b))
def sub(a, b): return tuple(x - y for x, y in zip(a, b))
def scale(a, s): return tuple(x * s for x in a)
def dot(a, b): return sum(x * y for x, y in zip(a, b))
def length(a): return math.sqrt(dot(a, a))
def unit(a):
    n = length(a)
    if n < 1e-9: raise ResourceError('DEGENERATE_ANATOMICAL_AXIS', {'vector': a})
    return scale(a, 1.0 / n)
def cross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def centroid(points):
    return tuple(sum(p[i] for p in points) / len(points) for i in range(3))


def identity(): return [[1., 0., 0., 0.], [0., 1., 0., 0.], [0., 0., 1., 0.], [0., 0., 0., 1.]]
def mul(a, b): return [[sum(a[i][k]*b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]
def point(m, p): return tuple(sum(m[i][j]*p[j] for j in range(3))+m[i][3] for i in range(3))
def position(m): return tuple(m[i][3] for i in range(3))
def inverse(m):
    result = identity()
    for i in range(3):
        for j in range(3): result[i][j] = m[j][i]
        result[i][3] = -sum(m[j][i]*m[j][3] for j in range(3))
    return result


def bind_matrix(xyz, translation):
    # xrCore/math/_matrix.h setXYZi -> setHPB(-y,-x,-z), stored as columns i,j,k.
    h, p, b = -xyz[1], -xyz[0], -xyz[2]
    sh, ch, sp, cp, sb, cb = math.sin(h), math.cos(h), math.sin(p), math.cos(p), math.sin(b), math.cos(b)
    cc, cs, sc, ss = ch*cb, ch*sb, sh*cb, sh*sb
    columns = ((cc-sp*ss, -cp*sb, sp*cs+sc), (sp*sc+cs, cp*cb, ss-sp*cc), (-cp*sh, sp, cp*ch))
    result = identity()
    for i in range(3):
        for j in range(3): result[i][j] = columns[j][i]
        result[i][3] = translation[i]
    return result


def matrix_xyz(m):
    cy = math.sqrt(m[1][1]**2 + m[1][0]**2)
    if cy > 1e-7:
        h, p, b = -math.atan2(m[0][2], m[2][2]), -math.atan2(-m[1][2], cy), -math.atan2(m[1][0], m[1][1])
    else:
        h, p, b = -math.atan2(-m[2][0], m[0][0]), -math.atan2(-m[1][2], cy), 0.
    return (-p, -h, -b)


def validate_hierarchy(bones):
    names = [b['name'] for b in bones]
    if len(set(names)) != len(names): raise ResourceError('DUPLICATE_BONE_NAME', names)
    roots = [b['name'] for b in bones if not b['parent']]
    if len(roots) != 1: raise ResourceError('ROOT_COUNT', roots)
    mapping = {b['name']: b for b in bones}
    for bone in bones:
        visited, parent = {bone['name']}, bone['parent']
        while parent:
            if parent not in mapping: raise ResourceError('MISSING_PARENT', {'bone': bone['name'], 'parent': parent})
            if parent in visited: raise ResourceError('BONE_CYCLE', {'bone': bone['name'], 'parent': parent})
            visited.add(parent); parent = mapping[parent]['parent']
    return mapping


def _load_ogf(path, skeleton_only=False):
    path = Path(path); raw = path.read_bytes(); root = chunks(raw); data = dict(root)
    if data.get(1, b'\0')[0] != 4: raise ResourceError('OGF_VERSION', {'path': str(path)})
    names = data.get(13)
    if names is None or 16 not in data: raise ResourceError('MISSING_SKELETON', {'path': str(path)})
    count, = struct.unpack_from('<I', names); offset = 4; bones = []
    for _ in range(count):
        name, offset = zstring(names, offset); parent, offset = zstring(names, offset)
        bones.append({'name': name, 'parent': parent, 'obb_raw': names[offset:offset+60]}); offset += 60
    if offset != len(names): raise ResourceError('BONE_NAMES_SIZE', {'expected': offset, 'actual': len(names)})
    mapping = validate_hierarchy(bones); ik = data[16]; offset = 0
    for bone in bones:
        start = offset; version, = struct.unpack_from('<I', ik, offset); offset += 4
        if version not in (0, 1): raise ResourceError('IK_VERSION', {'bone': bone['name'], 'version': version})
        material, offset = zstring(ik, offset); shape_offset = offset-start; offset += 112
        offset += 76 if version else 72
        bind_offset = offset-start; xyz = struct.unpack_from('<3f', ik, offset); trans = struct.unpack_from('<3f', ik, offset+12); offset += 40
        bone.update(rotation_xyz=xyz, translation=trans, bind_local=bind_matrix(xyz,trans), ik_raw=ik[start:offset], ik_bind_offset=bind_offset, ik_shape_offset=shape_offset)
    if offset != len(ik): raise ResourceError('IK_DATA_SIZE', {'expected': offset, 'actual': len(ik)})
    def global_bind(bone):
        if 'bind_global' not in bone:
            bone['bind_global'] = mul(global_bind(mapping[bone['parent']]), bone['bind_local']) if bone['parent'] else bone['bind_local']
        return bone['bind_global']
    for bone in bones: global_bind(bone)
    meshes=[]
    for child_id, payload in ([] if skeleton_only else chunks(data.get(9,b''))):
        child=chunks(payload); c=dict(child)
        if 3 not in c or 4 not in c: raise ResourceError('UNSUPPORTED_MESH_LAYOUT', {'child':child_id})
        fmt,n=struct.unpack_from('<II',c[3]); types={1:1,2:2,3:3,4:4,0x12071980:1,0x240e3300:2,0x481c6600:3,0x5a237f80:4}
        if fmt not in types: raise ResourceError('VERTEX_FORMAT', {'format':hex(fmt)})
        influences=types[fmt]; stride={1:60,2:64,3:70,4:76}[influences]; vertices=[]
        if len(c[3]) != 8+n*stride: raise ResourceError('VERTEX_DATA_SIZE', {'child':child_id})
        for i in range(n):
            vraw=c[3][8+i*stride:8+(i+1)*stride]; poff=0 if influences==1 else 2*influences
            geom=struct.unpack_from('<12f',vraw,poff)
            if influences==1: ids=[struct.unpack_from('<I',vraw,56)[0]];weights=[1.];uvoff=48
            else:
                ids=struct.unpack_from('<'+'H'*influences,vraw); weights=list(struct.unpack_from('<'+'f'*(influences-1),vraw,poff+48)); weights.append(1-sum(weights));uvoff=poff+48+4*(influences-1)
            actual={}
            for bi,w in zip(ids,weights):
                if bi>=count or not math.isfinite(w) or w < -1e-5: raise ResourceError('INVALID_WEIGHT', {'child':child_id,'vertex':i,'bone':bi,'weight':w})
                if w>0: actual[bi]=actual.get(bi,0)+w
            if not all(math.isfinite(v) for v in geom):raise ResourceError('NONFINITE_GEOMETRY', {'child':child_id,'vertex':i})
            vertices.append({'position':geom[:3],'normal':geom[3:6],'tangent':geom[6:9],'binormal':geom[9:12],'uv':struct.unpack_from('<2f',vraw,uvoff),'weights':actual,'raw':vraw,'geometry_raw':vraw[poff:poff+48]+vraw[uvoff:uvoff+8]})
        ni,=struct.unpack_from('<I',c[4]);indices=struct.unpack_from('<'+'H'*ni,c[4],4)
        if len(c[4])!=4+ni*2 or ni%3 or max(indices,default=0)>=n:raise ResourceError('INVALID_INDICES', {'child':child_id})
        full=indices
        if 6 in c:
            windows,=struct.unpack_from('<I',c[6],16)
            if not windows or len(c[6])!=20+8*windows:raise ResourceError('INVALID_LOD_WINDOWS',{'child':child_id})
            start,tris,nverts=struct.unpack_from('<IHH',c[6],20);full=indices[start:start+tris*3]
            if len(full)!=tris*3:raise ResourceError('INVALID_LOD_RANGE',{'child':child_id})
        meshes.append({'child_id':child_id,'chunks':child,'vertices':vertices,'indices':indices,'full_detail_triangles':[full[i:i+3] for i in range(0,len(full),3)],'format':fmt,'influences':influences,'material':c.get(2,b'')})
    return {'path':path,'raw':raw,'chunks':root,'bones':bones,'meshes':meshes}


def load_ogf(path, skeleton_only=False):
    try:
        return _load_ogf(path,skeleton_only)
    except ResourceError:
        raise
    except (ValueError,KeyError,IndexError,struct.error) as error:
        raise ResourceError('INVALID_OGF_FORMAT',{'path':str(path),'reason':str(error)}) from error


def geometry_fingerprints(model):
    return [{'child_id':m['child_id'],'vertices':len(m['vertices']),'geometry_sha256':sha(b''.join(v['geometry_raw'] for v in m['vertices'])),'indices_sha256':sha(dict(m['chunks'])[4]),'material_sha256':sha(m['material']),'lod_sha256':sha(dict(m['chunks']).get(6,b''))} for m in model['meshes']]


def write_ogf(model, bones, new_weights):
    encoded=[]
    for mesh in model['meshes']:
        records=[];typ=mesh['influences'];limit=max(typ, max((len(new_weights.get((mesh['child_id'],i),v['weights'])) for i,v in enumerate(mesh['vertices'])),default=typ))
        if limit>4: raise ResourceError('WEIGHT_LIMIT', {'child':mesh['child_id'],'limit':limit})
        for i,vertex in enumerate(mesh['vertices']):
            weights=new_weights.get((mesh['child_id'],i))
            if weights is None and limit==typ:records.append(vertex['raw']);continue
            weights=weights or vertex['weights'];pairs=sorted(weights.items(),key=lambda x:(-x[1],x[0]));total=sum(w for _,w in pairs)
            pairs=[(bi,w/total) for bi,w in pairs];pairs += [(pairs[-1][0],0.)]*(limit-len(pairs))
            if limit==1:record=vertex['geometry_raw']+struct.pack('<I',pairs[0][0])
            else:record=struct.pack('<'+'H'*limit,*(bi for bi,_ in pairs))+vertex['geometry_raw'][:48]+struct.pack('<'+'f'*(limit-1),*(w for _,w in pairs[:-1]))+vertex['geometry_raw'][48:]
            records.append(record)
        payload=struct.pack('<II',limit,len(records))+b''.join(records)
        encoded.append((mesh['child_id'],encode_chunks([(k,payload if k==3 else v) for k,v in mesh['chunks']])))
    names=struct.pack('<I',len(bones))+b''.join(b['name'].encode()+b'\0'+b['parent'].encode()+b'\0'+b['obb_raw'] for b in bones)
    ik=b''.join(b['ik_raw'] for b in bones);result=[]
    for key,payload in model['chunks']:
        if key in (14,15,19,24,23):continue  # Rigid pose receiver has no incompatible source OMF or skeleton LOD.
        if key==1:payload=payload[:1]+bytes([10])+payload[2:]
        elif key==9:payload=encode_chunks(encoded)
        elif key==13:payload=names
        elif key==16:payload=ik
        result.append((key,payload))
    return encode_chunks(result)
