#!/usr/bin/env python3
"""Validate effective IX-Ray animation bindings against runtime-resolved native metadata.

Input is zz_movement_library_dump's pSettings and FS snapshot, not an LTX merger.
The selected OGF/OMF chunks preserve native skeletons, partitions and motion defs;
keyframes and geometry are intentionally outside this library check. Only native
implicit item-idle selection is allowed; authored hand clips remain mandatory.
Native clip lookup searches referenced banks in reverse order.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import sys


class NativeReader:
    def __init__(self, data: bytes):
        self.data, self.pos = data, 0

    def read(self, fmt):
        size = struct.calcsize('<' + fmt)
        if self.pos + size > len(self.data):
            raise ValueError('truncated native field')
        result = struct.unpack_from('<' + fmt, self.data, self.pos)
        self.pos += size
        return result[0] if len(result) == 1 else result

    def string(self):
        end = self.data.index(0, self.pos)
        result = self.data[self.pos:end].decode('cp1251')
        self.pos = end + 1
        return result

    def line(self):
        end = self.data.index(10, self.pos)
        result = self.data[self.pos:end].rstrip(b'\r').decode('cp1251')
        self.pos = end + 1
        return result


def chunks(data):
    result, offset = {}, 0
    while offset < len(data):
        if offset + 8 > len(data):
            raise ValueError('truncated chunk header')
        key, length = struct.unpack_from('<II', data, offset)
        offset += 8
        if key & 0x80000000 or offset + length > len(data):
            raise ValueError(f'unsupported compressed/truncated chunk {key}')
        if key in result:
            raise ValueError(f'duplicate native chunk {key}')
        result[key] = data[offset:offset + length]
        offset += length
    return result


def resource_name(value, extension='.ogf'):
    value = value.strip().strip('"').replace('/', '\\').lower()
    if value and not Path(value).suffix:
        value += extension
    return value


def parse_native(data):
    """Native layouts: Fmesh.h, SkeletonMotions.cpp and OGFs bone-name chunk."""
    c = chunks(data)
    result = {'bones': [], 'refs': [], 'parts': [], 'motions': {}, 'errors': []}
    if 13 in c:
        r = NativeReader(c[13])
        for _ in range(r.read('I')):
            result['bones'].append({'name': r.string(), 'parent': r.string()})
            r.pos += 60  # OBB matrix, centre and halfsize.
        if r.pos != len(r.data):
            raise ValueError('invalid bone-name payload size')
    if 24 in c:
        r = NativeReader(c[24])
        result['refs'] = [resource_name(r.string(), '.omf') for _ in range(r.read('I'))]
    elif 19 in c:
        result['refs'] = [resource_name(x, '.omf') for x in NativeReader(c[19]).string().split(',') if x.strip()]
    if 15 in c:
        r = NativeReader(c[15])
        version = r.read('H')
        if not 1 <= version <= 4:
            raise ValueError(f'unsupported motion parameters version {version}')
        for _ in range(r.read('H')):
            part = {'name': r.string().lower(), 'bones': []}
            for _ in range(r.read('H')):
                part['bones'].append({'name': r.string().lower(), 'index': r.read('I')})
            result['parts'].append(part)
        for _ in range(r.read('H')):
            name, flags = r.string().lower(), r.read('I')
            part, index = r.read('HH')
            speed, power, accrue, falloff = r.read('ffff')
            motion = {'flags': flags, 'bone_or_part': part, 'index': index, 'speed': speed,
                      'power': power, 'accrue': accrue, 'falloff': falloff, 'marks': []}
            if version >= 4:
                for _ in range(r.read('I')):
                    mark = {'name': r.line(), 'intervals': []}
                    for _ in range(r.read('I')):
                        mark['intervals'].append(list(r.read('ff')))
                    motion['marks'].append(mark)
            if name in result['motions']:
                result['errors'].append(f'duplicate motion definition: {name}')
            result['motions'].setdefault(name, motion)
            if not all(math.isfinite(x) for x in (speed, power, accrue, falloff)) or speed <= 0:
                result['errors'].append(f'invalid motion speed/parameters: {name}')
            if not flags & 1 and part != 0xffff and part >= len(result['parts']):
                result['errors'].append(f'invalid cycle partition: {name} -> {part}')
            for mark in motion['marks']:
                for begin, end in mark['intervals']:
                    if not math.isfinite(begin + end) or begin < 0 or end < begin:
                        result['errors'].append(f'invalid mark interval: {name} -> {mark["name"]}')
        if r.pos != len(r.data):
            raise ValueError('unread motion parameter bytes')
    return result


def decode_text(path):
    raw = path.read_bytes()
    try:
        return raw.decode('utf-8-sig')
    except UnicodeDecodeError:
        return raw.decode('cp1251')


def read_rows(path):
    return [json.loads(line) for line in decode_text(path).splitlines() if line.strip()]


class Library:
    def __init__(self, directory, profile):
        self.directory = directory
        self.receipts = {r['virtual']: r for r in read_rows(directory / 'vfs.ndjson')}
        self.cache, self.models, self.issues = {}, {}, []
        self.profile = profile

    def issue(self, code, chain, **detail):
        self.issues.append({'code': code, 'chain': chain, **detail})

    def asset(self, name):
        if name in self.cache:
            return self.cache[name]
        receipt = self.receipts.get(name)
        if not receipt or receipt.get('status') != 'metadata_exported':
            self.cache[name] = None
            self.issue('RESOURCE_UNRESOLVED', name, receipt=receipt)
            return None
        try:
            data = (self.directory / receipt['metadata']).read_bytes()
            value = parse_native(data)
            value['metadata_sha256'] = hashlib.sha256(data).hexdigest()
            for motion in value['motions'].values():
                payload = receipt.get('motions', {}).get(str(motion['index']))
                motion['sample_header'] = payload
                if payload is None:
                    value['errors'].append(f'motion payload absent: index {motion["index"]}')
            self.cache[name] = value
            for error in value['errors']:
                self.issue('RESOURCE_METADATA_INVALID', name, error=error)
            return value
        except (ValueError, KeyError, IndexError, struct.error, OSError) as error:
            self.cache[name] = None
            self.issue('RESOURCE_PARSE_ERROR', name, error=str(error))
            return None

    def model(self, name):
        if name in self.models:
            return self.models[name]
        asset = self.asset(name)
        if asset is None:
            return None
        result = {'virtual': name, 'bones': asset['bones'], 'motions': {}, 'part_names': set(),
                  'banks': asset['refs'] or ([name] if asset['motions'] else []), 'compatible': True}
        names = [x['name'] for x in result['bones']]
        if len(set(names)) != len(names):
            result['compatible'] = False
            self.issue('DUPLICATE_BONES', name)
        parents = {x['name']: x['parent'] for x in result['bones']}
        for bone in names:
            seen, cursor = set(), bone
            while cursor:
                if cursor in seen or cursor not in parents:
                    result['compatible'] = False
                    self.issue('INVALID_HIERARCHY', name, bone=bone)
                    break
                seen.add(cursor)
                cursor = parents[cursor]
        for bank in result['banks']:
            motion_asset = self.asset(bank)
            if motion_asset is None:
                result['compatible'] = False
                continue
            bank_bones = [b for part in motion_asset['parts'] for b in part['bones']]
            if sorted(b['name'] for b in bank_bones) != sorted(names) or sorted(b['index'] for b in bank_bones) != list(range(len(names))):
                result['compatible'] = False
                self.issue('BANK_SKELETON_MISMATCH', f'{name} -> {bank}', model_bones=len(names), bank_bones=len(bank_bones))
            result['part_names'].update(p['name'] for p in motion_asset['parts'])
            # ID_Cycle_Safe searches the last referenced bank first.
            for clip, definition in motion_asset['motions'].items():
                if not definition['flags'] & 1:
                    result['motions'][clip] = {'bank': bank, **definition}
        self.models[name] = result
        return result

    def compatible_profile(self, model):
        if not self.profile:
            return {'status': 'PROFILE_UNCHECKED'}
        signature = {(b['name'], b['parent']) for b in model['bones']}
        for key in ('donor42', 'actor47', 'target61'):
            expected = self.profile.get(key, [])
            if expected and signature == {(b['name'], b['parent']) for b in expected}:
                return {'status': 'MATCH', 'profile': key}
        self.issue('HUMANOID_PROFILE_MISMATCH', model['virtual'], bones=len(model['bones']))
        return {'status': 'MISMATCH', 'bones': len(model['bones'])}


def validate(directory, profile):
    complete = json.loads(decode_text(directory / 'complete.json'))
    if complete.get('status') != 'complete' or complete.get('schema') != 'humanoid_live_library_dump_v1':
        raise ValueError('runtime dump has no successful completion receipt')
    sections = {row['section']: row['values'] for row in read_rows(directory / 'effective.ndjson')}
    if len(sections) != complete['sections']:
        raise ValueError('effective section count does not match completion receipt')
    lib = Library(directory, profile)
    if len(lib.receipts) != complete['resources']:
        raise ValueError('VFS resource count does not match completion receipt')
    items, actions, hand_sections, hands = {}, {}, {'actor_hud'}, set()
    for section, values in sections.items():
        if 'class' in values and ('inv_name' in values or 'hud' in values):
            items[section] = values
        if any(k.startswith(('anm_', 'anim_')) for k in values):
            actions[section] = values
        for key in ('player_hud', 'player_hud_section'):
            if values.get(key) in sections:
                hand_sections.add(values[key])
        if 'hands' in section and 'item_visual' not in values:
            for value in values.values():
                name = resource_name(value)
                if name in lib.receipts:
                    hands.add(name)
    for section in hand_sections:
        value = sections.get(section, {}).get('visual')
        if value:
            hands.add(resource_name(value))
    hand_groups = {}
    for hand in sorted(hands):
        model = lib.model(hand)
        if model is None:
            continue
        signature = (tuple((b['name'], b['parent']) for b in model['bones']), tuple(model['banks']))
        if signature not in hand_groups:
            hand_groups[signature] = {'id': len(hand_groups), 'models': [], 'model': model,
                                      'profile': lib.compatible_profile(model)}
        hand_groups[signature]['models'].append(hand)
    if not hand_groups:
        lib.issue('NO_SOURCE_HAND_MODELS', 'actor/outfit player_hud -> visual')
    owners = defaultdict(set)
    item_rows = []
    for item, values in sorted(items.items()):
        links, pending, visited, reachable = {}, [item], set(), set()
        while pending:
            current = pending.pop()
            if current in visited:
                continue
            visited.add(current)
            if current in actions:
                owners[current].add(item)
                reachable.add(current)
            for key, value in sections[current].items():
                if any(token in key for token in ('hud', 'animator', 'scope', 'silencer', 'grenade', 'attach', 'visual')):
                    links[current + '/' + key] = value
                    for target in (x.strip() for x in value.split(',')):
                        if target in sections and target not in visited:
                            pending.append(target)
        item_rows.append({'item': item, 'class': values['class'], 'bindings': links,
                          'action_sections': sorted(reachable), 'has_action_section': bool(reachable)})
    coverage = []
    for section, values in sorted(actions.items()):
        visual = values.get('item_visual') or values.get('visual')
        item_model = lib.model(resource_name(visual)) if visual else None
        combined = 'item_visual' not in values and bool(visual)
        sources = [{'id': 'combined', 'models': [resource_name(visual)], 'model': item_model}] if combined and item_model else list(hand_groups.values())
        for alias, value in sorted(values.items()):
            if not alias.startswith(('anm_', 'anim_')):
                continue
            if alias.startswith('anim_') and not combined:
                continue  # Native legacy anim_ is ignored when using separate global hands.
            tokens = [part.strip() for part in value.split(',')]
            base = tokens[0].lower()
            bonepart = alias.startswith('anm_bp_')
            explicit_item = not bonepart and len(tokens) > 1 and bool(tokens[1])
            paired = (tokens[1] if len(tokens) > 1 and tokens[1] else base).lower()
            try:
                speed = 1.0 if bonepart or len(tokens) < 3 or not tokens[2] else float(tokens[2])
            except ValueError:
                speed = float('nan')
            partitions = tokens[1:] if bonepart else tokens[3:]
            chain = f'{",".join(sorted(owners[section])) or "<standalone-action>"} -> {section} -> {alias}'
            if not math.isfinite(speed) or speed <= 0:
                lib.issue('ALIAS_SPEED_INVALID', chain, value=value)
            active_sources = [{'id': 'item_part', 'models': [resource_name(visual)], 'model': item_model}] if bonepart and item_model else sources
            if not active_sources:
                lib.issue('SOURCE_MODEL_UNRESOLVED', chain)
            for source in active_sources:
                model = source['model']
                variants = [base] + [base + str(i) for i in range(1, 9)]
                found = [variant for variant in variants if variant in model['motions']]
                row = {'chain': chain, 'section': section, 'alias': alias, 'original': value,
                       'source_group': source['id'], 'alias_speed': speed if math.isfinite(speed) else None,
                       'item_binding': 'EXPLICIT' if explicit_item else 'NATIVE_IMPLICIT',
                       'partitions': partitions, 'variants': []}
                if base not in found:
                    lib.issue('ORIGINAL_HAND_CLIP_MISSING' if not bonepart else 'ORIGINAL_ITEM_PART_CLIP_MISSING', chain, clip=base, sources=source['models'])
                for part in partitions:
                    if part and item_model and part.lower() not in item_model['part_names']:
                        lib.issue('ITEM_PARTITION_MISSING', chain, partition=part, model=item_model['virtual'])
                for variant in found:
                    definition = model['motions'][variant]
                    item_clip = variant if paired == base else paired
                    pair = item_model['motions'].get(item_clip) if item_model else None
                    item_selection = 'EXPLICIT_CLIP' if explicit_item else 'IMPLICIT_MATCHING_CLIP'
                    if not bonepart and item_model and item_model['banks'] and pair is None:
                        if explicit_item:
                            lib.issue('ITEM_CLIP_MISSING', chain, hand_clip=variant, item_clip=item_clip, model=item_model['virtual'])
                            item_selection = 'EXPLICIT_CLIP_MISSING'
                        else:
                            item_clip = 'idle'
                            pair = item_model['motions'].get(item_clip)
                            item_selection = 'IMPLICIT_HAND_ONLY_ITEM_IDLE'
                            if pair is None:
                                lib.issue('IMPLICIT_ITEM_IDLE_MISSING', chain, hand_clip=variant, model=item_model['virtual'])
                                item_selection = 'IMPLICIT_ITEM_IDLE_MISSING'
                    elif not item_model or not item_model['banks']:
                        item_clip = None
                        item_selection = 'HAND_ONLY_STATIC_ITEM' if item_model else 'NO_ITEM_MODEL'
                    row['variants'].append({'hands_clip': variant, 'hands': definition,
                        'item_clip': None if bonepart else item_clip, 'item': pair,
                        'item_selection': 'ITEM_BONEPART_CLIP' if bonepart else item_selection,
                        'effective_hands_speed': speed * definition['speed'] if math.isfinite(speed) else None})
                coverage.append(row)
    public_groups = [{k: v for k, v in group.items() if k != 'model'} for group in hand_groups.values()]
    return {'schema': 'humanoid_animation_library_v1', 'evidence': 'RUNTIME_PSETTINGS_AND_VFS_METADATA',
            'runtime_receipt': complete, 'status': 'METADATA_ERRORS' if lib.issues else 'METADATA_VALIDATED',
            'limitations': ['No geometry, motion-key evaluation, contact/grip, rendering or event execution is validated.',
                'All effective item/action definitions are covered; presence is not proof of current player ownership or acquisition.',
                'Runtime Lua section modifiers are recorded in effective config but their return values require action execution.',
                'Metadata hashes identify exported chunks, not original full OGF/OMF bytes.'],
            'counts': {'items': len(items), 'action_sections': len(actions), 'source_hand_models': len(hands),
                       'hand_groups': len(hand_groups), 'bindings': len(coverage),
                       'variants': sum(len(r['variants']) for r in coverage), 'issues': len(lib.issues)},
            'issue_counts': dict(Counter(x['code'] for x in lib.issues)), 'hand_groups': public_groups,
            'items': item_rows, 'coverage': coverage, 'issues': lib.issues,
            'resources': [{'virtual': name, 'metadata_sha256': value['metadata_sha256'],
                           'bones': len(value['bones']), 'refs': value['refs']} for name, value in lib.cache.items() if value]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dump', type=Path)
    parser.add_argument('--profile', type=Path)
    parser.add_argument('--inspect', type=Path, help='Read one native OGF/OMF metadata file without a runtime claim.')
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    if args.inspect:
        result = {'evidence': 'OFFLINE_NATIVE_RESOURCE_ONLY', 'path': str(args.inspect),
                  'sha256': hashlib.sha256(args.inspect.read_bytes()).hexdigest(), **parse_native(args.inspect.read_bytes())}
    elif args.dump:
        profile = json.loads(decode_text(args.profile)) if args.profile else None
        result = validate(args.dump, profile)
    else:
        parser.error('--dump or --inspect is required')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    print(json.dumps({'output': str(args.out), 'status': result.get('status'), 'counts': result.get('counts'),
                      'issues': result.get('issue_counts')}, ensure_ascii=False))
    return 2 if result.get('issues') else 0


if __name__ == '__main__':
    sys.exit(main())
