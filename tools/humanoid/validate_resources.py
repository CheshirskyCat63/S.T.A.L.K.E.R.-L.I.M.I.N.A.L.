"""Validate prepared humanoid resource bytes without starting the engine."""
import argparse
import json
import math
from pathlib import Path
from ogf_binary import ResourceError, geometry_fingerprints, identity, inverse, load_ogf, mul, point, sha


def validate_pair(source,target,profile,evidence):
    expected=[(b['name'],b['parent']) for b in profile['target61']]
    if [(b['name'],b['parent']) for b in target['bones']]!=expected:
        raise ResourceError('TARGET_CONTRACT_MISMATCH',{'count':len(target['bones'])})
    root=dict(target['chunks'])
    if root[1][1]!=10 or any(k in root for k in (14,15,19,24,23)):
        raise ResourceError('TARGET_NOT_RIGID_RECEIVER',{'type':root[1][1]})
    if geometry_fingerprints(source)!=geometry_fingerprints(target):
        raise ResourceError('SURFACE_BYTES_CHANGED',{'source':geometry_fingerprints(source),'target':geometry_fingerprints(target)})
    if dict(source['chunks'])[1][4:]!=root[1][4:]:raise ResourceError('MODEL_BOUNDS_CHANGED',{})
    regions={(r['child'],r['vertex']):r['region'] for r in evidence['modified_vertices']};allowed=set(regions)
    influence_counts={b['name']:0 for b in target['bones'][47:]};neutral_error=0.;weight_error=0.;changed=0
    neutral_matrices=[mul(b['bind_global'],inverse(b['bind_global'])) for b in target['bones']]
    for sm,tm in zip(source['meshes'],target['meshes']):
        for vi,(sv,tv) in enumerate(zip(sm['vertices'],tm['vertices'])):
            ref=(sm['child_id'],vi);weight_error=max(weight_error,abs(sum(tv['weights'].values())-1.))
            if len(tv['weights'])>4 or any(not math.isfinite(w) or w<0 for w in tv['weights'].values()):
                raise ResourceError('INVALID_TARGET_WEIGHTS',{'child':sm['child_id'],'vertex':vi})
            if ref not in allowed:
                for bi in set(sv['weights'])|set(tv['weights']):
                    if abs(sv['weights'].get(bi,0.)-tv['weights'].get(bi,0.))>1e-6:
                        raise ResourceError('WEIGHTS_CHANGED_OUTSIDE_REGION',{'child':sm['child_id'],'vertex':vi,'bone':bi})
            else:changed+=1
            position=[0.,0.,0.]
            for bi,w in tv['weights'].items():
                transformed=point(neutral_matrices[bi],tv['position'])
                for axis in range(3):position[axis]+=w*transformed[axis]
                if bi>=47 and w>0.0001:
                    name=target['bones'][bi]['name'];influence_counts[name]+=1
                    anatomical=name.removeprefix('bip01_')
                    expected_region=anatomical if 'twist' in anatomical else anatomical[:9]
                    if regions.get(ref)!=expected_region:
                        raise ResourceError('CROSS_ANATOMICAL_WEIGHT',{'bone':name,'region':regions.get(ref),'child':sm['child_id'],'vertex':vi})
            neutral_error=max(neutral_error,max(abs(a-b) for a,b in zip(position,sv['position'])))
    if neutral_error>1e-6 or weight_error>1e-6:raise ResourceError('NEUTRAL_SKINNING_ERROR',{'position_metres':neutral_error,'weights':weight_error})
    if any(count==0 for count in influence_counts.values()):raise ResourceError('ADDED_BONE_WITHOUT_SURFACE',influence_counts)
    if changed!=len(allowed):raise ResourceError('INVALID_MODIFIED_VERTEX_RECEIPT',{'actual':changed,'reported':len(allowed)})
    return {'status':'PASS_STATIC','bone_count':61,'rigid_receiver':True,'geometry_uv_normals_indices_materials_lod_identical':True,
            'neutral_max_error_metres':neutral_error,'weight_sum_max_error':weight_error,'max_influences':4,
            'modified_vertices':changed,'weights_outside_regions_preserved':True,'added_bones_confined_to_anatomical_regions':True,'added_bone_influence_vertices':influence_counts,
            'limitations':['Runtime pose transfer, collision correspondence, visual action quality and all other outfit surfaces are separate acceptance gates.']}


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('manifest',type=Path);parser.add_argument('--check-determinism',action='store_true');args=parser.parse_args()
    manifest=json.loads(args.manifest.read_text('utf-8'));profile_path=Path(manifest['profile']['path']);profile=json.loads(profile_path.read_text('utf-8'))
    if sha(profile_path.read_bytes())!=manifest['profile']['sha256']:raise ResourceError('PROFILE_HASH_MISMATCH',{})
    for source in manifest['sources'].values():
        if sha(Path(source['path']).read_bytes())!=source['sha256']:raise ResourceError('SOURCE_HASH_MISMATCH',{'path':source['path']})
    if manifest['status']!='PREPARED_STATIC_VALIDATED':raise ResourceError('MANIFEST_REJECTED',manifest['rejections'])
    results=[]
    for entry in manifest['models']:
        source=load_ogf(entry.get('source',manifest['sources'].get('actor'))['path'])
        path=Path(entry['path'])
        if sha(path.read_bytes())!=entry['sha256']:raise ResourceError('TARGET_HASH_MISMATCH',{'path':str(path)})
        result=validate_pair(source,load_ogf(path),profile,entry['preparation'])
        if args.check_determinism:
            from prepare_resources import prepare_model
            repeated,_=prepare_model(source,profile)
            if repeated!=path.read_bytes():raise ResourceError('NONDETERMINISTIC_PREPARATION',{})
            result['deterministic_repeat_identical']=True
        results.append(result)
    print(json.dumps({'status':'PASS_STATIC','models':len(results),'results':results},ensure_ascii=False))


if __name__=='__main__':main()
