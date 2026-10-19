"""Summarize session-local D3D9 metadata. This does not identify semantic objects."""
import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path


def summarize(path):
    draws = {}
    textures = defaultdict(lambda: {'draws': set(), 'slots': set(), 'sizes': set(), 'formats': set()})
    shaders = defaultdict(set)
    required = {'draw', 'kind', 'count', 'vertex_shader', 'pixel_shader', 'texture_id',
                'texture_slot', 'texture_width', 'texture_height', 'texture_format', 'skip_request'}
    with path.open(encoding='utf-8-sig', newline='') as handle:
        reader = csv.DictReader(handle)
        if not required <= set(reader.fieldnames or []):
            raise ValueError('Missing capture CSV fields')
        for row in reader:
            index = int(row['draw'])
            draws[index] = {'kind': row['kind'], 'count': int(row['count']),
                            'skip_request': int(row['skip_request'])}
            shaders[(row['vertex_shader'], row['pixel_shader'])].add(index)
            identity = row['texture_id']
            if int(identity, 0) == 0:
                continue
            group = textures[identity]
            group['draws'].add(index)
            group['slots'].add(int(row['texture_slot']))
            group['sizes'].add((int(row['texture_width']), int(row['texture_height'])))
            group['formats'].add(int(row['texture_format']))
    groups = []
    for identity, values in textures.items():
        indices = sorted(values['draws'])
        groups.append(dict(texture_id=identity, draw_count=len(indices), first_draw=indices[0],
                           last_draw=indices[-1], example_draws=indices[:20],
                           slots=sorted(values['slots']), sizes=sorted(values['sizes']),
                           formats=sorted(values['formats'])))
    groups.sort(key=lambda group: (-group['draw_count'], group['texture_id']))
    return dict(file=str(path), captured_draws=len(draws), unique_textures=len(groups),
                skip_requested_draws=sum(draw['skip_request'] != 0 for draw in draws.values()),
                texture_groups=groups,
                shader_groups=[dict(vertex_shader=vs, pixel_shader=ps, draw_count=len(indices))
                               for (vs, ps), indices in shaders.items()],
                limits='Handles are session-local and may be reused; metadata does not identify trees/characters. '
                       'Check the companion meta.txt for truncation and screenshots for visual context.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.capture), ensure_ascii=False, indent=2))
