"""Generate embedded native descriptor JSON from its exact contract fixture."""
from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--symbol', required=True)
    parser.add_argument('--delimiter', required=True)
    parser.add_argument('--chunks', type=int, required=True)
    args = parser.parse_args()
    fixture = json.loads(args.fixture.read_text(encoding='utf-8-sig'))
    descriptors = fixture['descriptors']
    counts = {
        'descriptor_count': len(descriptors),
        'query_count': sum(item['operation_kind'] == 'query' for item in descriptors),
        'operation_count': sum(item['operation_kind'] == 'operation' for item in descriptors),
    }
    assert counts['query_count'] + counts['operation_count'] == counts['descriptor_count']
    assert args.symbol.isidentifier() and args.delimiter.isidentifier() and args.chunks > 0
    text = json.dumps({**fixture, **counts}, separators=(',', ':'), ensure_ascii=True)
    width = (len(text) + args.chunks - 1) // args.chunks
    chunks = [text[index * width:(index + 1) * width] for index in range(args.chunks)]
    assert json.loads(''.join(chunks)) == {**fixture, **counts}
    lines = [
        '// Generated from the exact production contract fixture. Do not hand-edit.',
        f'static constexpr int32 {args.symbol}ChunkCount = {args.chunks};',
    ]
    for index, chunk in enumerate(chunks):
        delimiter = f'{args.delimiter}{index}'
        assert f'){delimiter}"' not in chunk
        lines.append(f'static constexpr ANSICHAR {args.symbol}Json{index}[] = R"{delimiter}({chunk}){delimiter}";')
    args.output.write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='')
    print(json.dumps(counts))


if __name__ == '__main__':
    main()
