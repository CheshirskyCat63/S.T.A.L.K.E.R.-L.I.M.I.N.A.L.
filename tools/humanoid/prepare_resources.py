"""Prepare a rigid humanoid61 receiver from a verified native47 surface.

No original file, OMF, ANM, installed configuration, or save is modified.
Topology failure produces a deterministic rejection manifest and no runtime mapping.
"""
from __future__ import annotations
import argparse
import copy
import heapq
import json
import math
import struct
from pathlib import Path
from ogf_binary import (ResourceError, add, centroid, cross, dot, encode_chunks, geometry_fingerprints,
                        identity, inverse, length, load_ogf, matrix_xyz, mul, point, position,
                        scale, sha, sub, unit, validate_hierarchy, write_ogf)


def check_contract(actual, expected, label):
    validate_hierarchy(actual)
    if [(b['name'], b['parent']) for b in actual] != [(b['name'], b['parent']) for b in expected]:
        raise ResourceError('RIG_CONTRACT_MISMATCH', {'contract': label, 'actual_count':len(actual),'expected_count':len(expected),
                            'actual':[(b['name'],b['parent']) for b in actual]})


def check_profile(profile):
    for key,count in (('actor47',47),('donor42',42),('target61',61)):
        if len(profile[key])!=count:raise ResourceError('PROFILE_BONE_COUNT',{'contract':key,'count':len(profile[key])})
        validate_hierarchy(profile[key])
    links=profile['donor_to_target'];sources=[x['source'] for x in links];targets=[x['target'] for x in links]
    required={x['name'] for x in profile['donor42']}-{profile['source_root'],profile['source_item_anchor']}
    if len(set(sources))!=len(sources) or len(set(targets))!=len(targets) or set(sources)!=required:
        raise ResourceError('PROFILE_MAPPING_NOT_BIJECTIVE',{'sources':sources,'targets':targets})
    if not set(targets)<={x['name'] for x in profile['target61']}:
        raise ResourceError('PROFILE_TARGET_UNKNOWN',{'targets':targets})
    if profile['source_root'] in sources or profile['source_item_anchor'] in sources:
        raise ResourceError('PROFILE_ROOT_OR_ANCHOR_IS_JOINT',{})


def combine_surface_patches(components, nodes, projections, base, lateral, normal):
    """Associate detached short overlays with one already separate finger surface.

    This never splits a connected surface or changes mesh connectivity. A patch
    must fit inside a longer component's longitudinal extent, overlap its lateral
    section, and have exactly one geometrically compatible host. Ambiguous cases
    remain separate and cannot pass the four-digit gate.
    """
    if len(components)<=4:return components,[]
    bounds=[]
    for component in components:
        values=[[projections[k],dot(sub(nodes[k]['position'],base),lateral),dot(sub(nodes[k]['position'],base),normal)] for k in component]
        bounds.append([(min(v[a] for v in values),max(v[a] for v in values)) for a in range(3)])
    hosts={}
    for pi,patch in enumerate(bounds):
        candidates=[];p_length=patch[0][1]-patch[0][0];p_width=patch[1][1]-patch[1][0]
        for hi,host in enumerate(bounds):
            if pi==hi:continue
            h_length=host[0][1]-host[0][0];h_width=host[1][1]-host[1][0]
            if h_length<2*p_length or min(p_width,h_width)<1e-6:continue
            # A cut crosses independently tessellated surfaces at different rows.
            # Permit one tenth of the host span at the proximal cut edge only.
            if patch[0][0]<host[0][0]-.1*h_length or patch[0][1]>host[0][1]-.2*h_length:continue
            overlap=min(patch[1][1],host[1][1])-max(patch[1][0],host[1][0])
            center_delta=abs(sum(patch[1])-sum(host[1]))*.5
            normal_gap=max(patch[2][0]-host[2][1],host[2][0]-patch[2][1],0.)
            if overlap<.8*min(p_width,h_width) or center_delta>.25*h_width or normal_gap>.25*h_width:continue
            candidates.append(hi)
        if len(candidates)==1:hosts[pi]=candidates[0]
    # A host must itself be a full branch, not another provisional overlay.
    hosts={pi:hi for pi,hi in hosts.items() if hi not in hosts}
    combined={i:list(c) for i,c in enumerate(components) if i not in hosts};evidence=[]
    for pi,hi in sorted(hosts.items()):
        combined[hi].extend(components[pi])
        evidence.append({'patch_vertices':len(components[pi]),'host_vertices':len(components[hi]),
                         'patch_bounds_forward_lateral_normal':bounds[pi],'host_bounds_forward_lateral_normal':bounds[hi]})
    return [sorted(c) for c in combined.values()],evidence


def hand_geometry(model, side, policy):
    bones=model['bones'];by_name={b['name']:i for i,b in enumerate(bones)};prefix=f'bip01_{side}_'
    first=[by_name[prefix+'finger1'+s] for s in ('','1','2')]
    group=[by_name[prefix+'finger2'+s] for s in ('','1','2')]
    thumb=[by_name[prefix+'finger0'+s] for s in ('','1','2')]
    base=position(bones[group[0]]['bind_global']);first_base=position(bones[first[0]]['bind_global'])
    forward=unit(sub(position(bones[group[2]]['bind_global']),base))
    lateral=unit(sub(sub(base,first_base),scale(forward,dot(sub(base,first_base),forward))))
    normal=unit(cross(forward,lateral));epsilon=policy['weld_epsilon_metres']
    def key(p):return tuple(round(x/epsilon) for x in p)
    nodes={};vertex_nodes={};adj={}
    for mesh in model['meshes']:
        for vi,v in enumerate(mesh['vertices']):
            w=v['weights']
            if sum(w.get(i,0.) for i in first+group)<=0.03 or sum(w.get(i,0.) for i in thumb)>=0.5:continue
            k=key(v['position']);nodes.setdefault(k,{'position':v['position'],'refs':[]});nodes[k]['refs'].append((mesh['child_id'],vi))
            vertex_nodes[(mesh['child_id'],vi)]=k;adj.setdefault(k,set())
        for tri in mesh['full_detail_triangles']:
            ks=[vertex_nodes[(mesh['child_id'],vi)] for vi in tri if (mesh['child_id'],vi) in vertex_nodes]
            for a in ks:
                for b in ks:
                    if a!=b:adj[a].add(b)
    if not nodes:raise ResourceError('HAND_SURFACE_ABSENT',{'side':side})
    projections={k:dot(sub(n['position'],base),forward) for k,n in nodes.items()}
    lo,hi=min(projections.values()),max(projections.values());span=hi-lo
    if span<0.02:raise ResourceError('HAND_SURFACE_SCALE',{'side':side,'span':span})
    samples=[];valid=[];minimum=policy['minimum_distal_welded_vertices']
    for step in range(12,39):
        fraction=step/50.;cut=lo+span*fraction;remaining={k for k,t in projections.items() if t>cut};components=[]
        while remaining:
            todo=[min(remaining)];component=[]
            while todo:
                k=todo.pop()
                if k not in remaining:continue
                remaining.remove(k);component.append(k);todo.extend(adj[k]&remaining)
            if len(component)>=minimum:components.append(sorted(component))
        raw_sizes=sorted([len(c) for c in components],reverse=True)
        components,patches=combine_surface_patches(components,nodes,projections,base,lateral,normal)
        samples.append({'fraction':fraction,'cut_metres_from_group_root':cut,'component_sizes':raw_sizes,
                        'digit_surface_sizes':sorted([len(c) for c in components],reverse=True),'detached_surface_patches':patches})
        if len(components)==4:valid.append((step,cut,components))
    stable=[]
    for i in range(len(valid)-policy['stable_cut_count']+1):
        run=valid[i:i+policy['stable_cut_count']]
        if [x[0] for x in run]!=list(range(run[0][0],run[0][0]+len(run))):continue
        consistent=True
        for earlier,later in zip(run,run[1:]):
            parents=[set(c) for c in earlier[2]];matches=[]
            for component in later[2]:
                members=set(component);overlaps=[pi for pi,parent in enumerate(parents) if members&parent]
                if len(overlaps)!=1 or not members<=parents[overlaps[0]]:consistent=False;break
                matches.append(overlaps[0])
            if len(set(matches))!=4:consistent=False
        if consistent:stable.append(run)
    if not stable:
        raise ResourceError('GEOMETRY_DIGIT_TOPOLOGY_UNCONFIRMED',{'side':side,'required_distal_branches':4,'samples':samples,
                            'explanation':'No stable, unambiguous four-digit non-thumb surface assignment at full-detail LOD. This is an inconclusive geometry gate, not proof that fingers are mathematically inseparable.'})
    # Prefer the earliest stable split: it retains enough geometry to fit the entire fingers.
    step,cut,components=stable[0][1]
    components.sort(key=lambda c:dot(sub(centroid([nodes[k]['position'] for k in c]),base),lateral))
    meshes={m['child_id']:m for m in model['meshes']}
    index_scores=[]
    for c in components:
        refs=[ref for k in c for ref in nodes[k]['refs']]
        index_scores.append(sum(sum(meshes[mi]['vertices'][vi]['weights'].get(bi,0) for bi in first) for mi,vi in refs)/len(refs))
    if max(range(4),key=lambda i:index_scores[i])!=0 or index_scores[0]<0.25:
        raise ResourceError('DIGIT_ORDER_AMBIGUOUS',{'side':side,'index_scores':index_scores})
    # Multi-source geodesic ownership stays on the actual connected surface, including UV seams.
    owners={};distances={};queue=[]
    for digit,component in enumerate(components,1):
        for k in component:distances[k]=0.;owners[k]=digit;heapq.heappush(queue,(0.,digit,k))
    while queue:
        distance,digit,k=heapq.heappop(queue)
        if distance!=distances.get(k) or digit!=owners[k]:continue
        for neighbor in sorted(adj[k]):
            candidate=distance+length(sub(nodes[k]['position'],nodes[neighbor]['position']))
            if candidate+1e-12<distances.get(neighbor,float('inf')):
                distances[neighbor]=candidate;owners[neighbor]=digit;heapq.heappush(queue,(candidate,digit,neighbor))
    thumb_count=sum(1 for m in model['meshes'] for v in m['vertices'] if sum(v['weights'].get(i,0.) for i in thumb)>0.5)
    if thumb_count<minimum:raise ResourceError('THUMB_SURFACE_ABSENT',{'side':side,'vertices':thumb_count})
    digit_data=[]
    for digit,c in enumerate(components,1):
        positions=[nodes[k]['position'] for k in c];center=centroid(positions)
        digit_data.append({'digit':digit,'distal_welded_vertices':len(c),'center':center,'max_forward':max(projections[k] for k in c),'lateral':dot(sub(center,base),lateral)})
    return {'side':side,'base':base,'forward':forward,'lateral':lateral,'normal':normal,'group_ids':group,
            'nodes':nodes,'vertex_nodes':vertex_nodes,'owners':owners,'projections':projections,'digits':digit_data,
            'evidence':{'side':side,'thumb_vertices':thumb_count,'selected_cut_fraction':step/50.,'selected_cut_metres':cut,'digits':digit_data,'cut_samples':samples,'status':'SEPARATE_FINGER_SURFACES'}}


def set_bind(bone, parent_global, global_bind, new=False):
    local=mul(inverse(parent_global),global_bind);xyz=matrix_xyz(local);translation=position(local)
    raw=bytearray(bone['ik_raw']);offset=bone['ik_bind_offset'];struct.pack_into('<6f',raw,offset,*xyz,*translation)
    if new:
        shape=bone['ik_shape_offset'];struct.pack_into('<HH',raw,shape,0,5)  # no pickable/physics shape for added render joints
        struct.pack_into('<f',raw,offset+24,0.)
    bone.update(bind_local=local,bind_global=global_bind,rotation_xyz=xyz,translation=translation,ik_raw=bytes(raw))


def prepare_model(model, profile):
    check_contract(model['bones'],profile['actor47'],'actor47')
    bones=copy.deepcopy(model['bones']);names={b['name']:i for i,b in enumerate(bones)};hands=[hand_geometry(model,s,profile['geometry_policy']) for s in ('l','r')]
    # Append in profile order, retaining all original bone indices.
    for definition in profile['target61'][47:]:
        name=definition['name'];source=name.replace('finger3','finger2').replace('finger4','finger2').replace('_forearm_twist','_forearm')
        bone=copy.deepcopy(bones[names[source]]);bone['name']=name;bone['parent']=definition['parent'];names[name]=len(bones);bones.append(bone)
    for hand in hands:
        side=hand['side'];prefix=f'bip01_{side}_';base=hand['base'];forward=hand['forward'];lateral=hand['lateral'];source_chain=[model['bones'][i] for i in hand['group_ids']]
        middle_extent=hand['digits'][1]['max_forward']
        if middle_extent<=0:raise ResourceError('MIDDLE_FINGER_EXTENT',hand['evidence'])
        for digit in (2,3,4):
            data=hand['digits'][digit-1]
            for joint,suffix in enumerate(('','1','2')):
                name=prefix+f'finger{digit}'+suffix;bone=bones[names[name]];global_bind=copy.deepcopy(source_chain[joint]['bind_global'])
                # Fit MCP/PIP/DIP pivots to the measured individual surface length.
                # The old finger2 chain represented three unequal fingers together.
                anatomical=add(add(base,scale(lateral,data['lateral'])),scale(forward,data['max_forward']*(0.,0.45,0.75)[joint]))
                for axis in range(3):global_bind[axis][3]=anatomical[axis]
                set_bind(bone,bones[names[bone['parent']]]['bind_global'],global_bind,digit>2)
        forearm=bones[names[prefix+'forearm']];wrist=bones[names[prefix+'hand']];twist=bones[names[prefix+'forearm_twist']]
        twist_global=copy.deepcopy(forearm['bind_global']);center=scale(add(position(forearm['bind_global']),position(wrist['bind_global'])),0.5)
        for axis in range(3):twist_global[axis][3]=center[axis]
        set_bind(twist,forearm['bind_global'],twist_global,True)
    changed={};changed_regions={};influence_count={b['name']:0 for b in bones[47:]}
    for mesh in model['meshes']:
        for vi,vertex in enumerate(mesh['vertices']):
            ref=(mesh['child_id'],vi);weights=dict(vertex['weights']);region=None
            for hand in hands:
                side=hand['side'];prefix=f'bip01_{side}_';key=hand['vertex_nodes'].get(ref)
                if key is not None and key in hand['owners'] and hand['projections'][key]>=-0.005:
                    digit=hand['owners'][key]
                    if digit==1:
                        # Preserve the independently authored index chain; eliminate only cross-digit group influence.
                        for source,suffix in zip(hand['group_ids'],('','1','2')):
                            amount=weights.pop(source,0.)
                            if amount:target=names[prefix+'finger1'+suffix];weights[target]=weights.get(target,0.)+amount
                    else:
                        amount=sum(weights.pop(names[prefix+f'finger{f}'+suffix],0.) for f in (1,2) for suffix in ('','1','2'))
                        extent=hand['digits'][digit-1]['max_forward'];t=max(0.,hand['projections'][key]/extent)
                        if t<0.45:joints=((0,1.-t/0.45),(1,t/0.45))
                        elif t<0.75:joints=((1,1.-(t-0.45)/0.30),(2,(t-0.45)/0.30))
                        else:joints=((2,1.),)
                        for joint,fraction in joints:
                            if fraction>1e-8 and amount:
                                target=names[prefix+f'finger{digit}'+('','1','2')[joint]];weights[target]=weights.get(target,0.)+amount*fraction
                    if weights!=vertex['weights']:region=side+'_finger'+str(digit)
                forearm_id=names[prefix+'forearm'];twist_id=names[prefix+'forearm_twist'];amount=weights.get(forearm_id,0.)
                if amount>0.01 and len(weights)<4:
                    start=position(bones[forearm_id]['bind_global']);end=position(bones[names[prefix+'hand']]['bind_global']);axis=sub(end,start);t=dot(sub(vertex['position'],start),axis)/dot(axis,axis)
                    if 0.05<t<0.95:
                        transfer=amount*0.5*t;weights[forearm_id]-=transfer;weights[twist_id]=transfer;region=side+'_forearm_twist'
            if weights!=vertex['weights']:
                if len(weights)>4:raise ResourceError('WEIGHT_LIMIT',{'child':mesh['child_id'],'vertex':vi,'weights':weights})
                changed[ref]=weights;changed_regions[ref]=region
            for bi,weight in weights.items():
                if bi>=47 and weight>0.0001:influence_count[bones[bi]['name']]+=1
    empty=[name for name,count in influence_count.items() if not count]
    if empty:raise ResourceError('ADDED_BONE_WITHOUT_SURFACE',{'bones':empty})
    # Update all rebound or added joint OBBs from their actual influenced surface in local bind space.
    rebound=set(range(47,len(bones)))|{names[f'bip01_{s}_finger2{j}'] for s in ('l','r') for j in ('','1','2')}
    for bi in rebound:
        inv=inverse(bones[bi]['bind_global']);points=[]
        for mesh in model['meshes']:
            for vi,vertex in enumerate(mesh['vertices']):
                if changed.get((mesh['child_id'],vi),vertex['weights']).get(bi,0.)>0.0001:points.append(point(inv,vertex['position']))
        if not points:raise ResourceError('REBOUND_BONE_WITHOUT_SURFACE',{'bone':bones[bi]['name']})
        lo=[min(p[a] for p in points) for a in range(3)];hi=[max(p[a] for p in points) for a in range(3)]
        center=[(a+b)*0.5 for a,b in zip(lo,hi)];half=[max((b-a)*0.5,0.000001) for a,b in zip(lo,hi)]
        bones[bi]['obb_raw']=struct.pack('<15f',1,0,0,0,1,0,0,0,1,*center,*half)
    check_contract(bones,profile['target61'],'target61')
    data=write_ogf(model,bones,changed)
    return data,{'hands':[h['evidence'] for h in hands],'added_bone_influence_vertices':influence_count,
                'modified_vertex_count':len(changed),'modified_vertices':[{'child':k[0],'vertex':k[1],'region':changed_regions[k]} for k in sorted(changed)],
                'rebound_original_bones':[bones[i]['name'] for i in sorted(rebound) if i<47],
                'geometry_fingerprints':geometry_fingerprints(model)}


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--source',type=Path,required=True);parser.add_argument('--donor',type=Path,required=True)
    parser.add_argument('--source-visual',required=True);parser.add_argument('--output-dir',type=Path,required=True);parser.add_argument('--profile',type=Path,default=Path(__file__).with_name('profile.json'))
    args=parser.parse_args();profile=json.loads(args.profile.read_text('utf-8'));args.output_dir.mkdir(parents=True,exist_ok=True)
    source_visual=args.source_visual.replace('/','\\').lower().removesuffix('.ogf');target_visual='liminal_humanoid\\'+source_visual
    if '..' in source_visual.split('\\') or ':' in source_visual or source_visual.startswith('\\'):raise ResourceError('INVALID_VISUAL_PATH',source_visual)
    manifest={'schema':'humanoid_resources_v1','profile':{'id':profile['id'],'path':str(args.profile.resolve()),'sha256':sha(args.profile.read_bytes())},
              'sources':{'actor':{'path':str(args.source.resolve()),'sha256':sha(args.source.read_bytes())},'donor':{'path':str(args.donor.resolve()),'sha256':sha(args.donor.read_bytes())}},'models':[],'rejections':[]}
    code=0
    try:
        check_profile(profile)
        donor=load_ogf(args.donor,skeleton_only=True);check_contract(donor['bones'],profile['donor42'],'donor42');source=load_ogf(args.source)
        data,evidence=prepare_model(source,profile);target=args.output_dir/'meshes'/Path(target_visual.replace('\\','/')+'.ogf');target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
        # Decode the writer output immediately; runtime mapping is emitted only after validation.
        from validate_resources import validate_pair
        validation=validate_pair(source,load_ogf(target),profile,evidence)
        manifest['models'].append({'source_visual':source_visual,'target_visual':target_visual,'path':str(target.resolve()),'sha256':sha(data),'validation':validation,'preparation':evidence})
    except ResourceError as error:
        code=2;manifest['rejections'].append({'source_visual':source_visual,'code':error.code,'detail':error.detail})
    manifest['status']='PREPARED_STATIC_VALIDATED' if not code else 'REJECTED'
    (args.output_dir/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2,sort_keys=True)+'\n',encoding='utf-8')
    config='; Generated only from statically validated rigid pose receivers.\n[liminal_humanoid_models]\n'+''.join(m['source_visual']+' = '+m['target_visual']+'\n' for m in manifest['models'])
    (args.output_dir/'mod_system_liminal_humanoid.ltx').write_text(config,encoding='utf-8')
    print(json.dumps({'status':manifest['status'],'models':len(manifest['models']),'rejections':manifest['rejections'],'manifest':str(args.output_dir/'manifest.json')},ensure_ascii=False))
    return code


if __name__=='__main__':raise SystemExit(main())
