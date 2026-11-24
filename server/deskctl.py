#!/usr/bin/env python3
"""Mac command line remote. No hardware button or non-stdlib package needed."""
import argparse
import json
import os
from pathlib import Path
import sys
from urllib.request import Request, urlopen
from urllib.error import HTTPError, URLError


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='http://127.0.0.1:8765')
    parser.add_argument('--token-file', type=Path, default=Path(__file__).resolve().parents[1] / '.local/token')
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('mode'); p.add_argument('value', choices=['work','fun'])
    p = sub.add_parser('page'); p.add_argument('value', choices=['quota','clock','loft','status','stock'])
    p = sub.add_parser('brightness'); p.add_argument('value', type=int, choices=range(8,161), metavar='8..160')
    p = sub.add_parser('message'); p.add_argument('value')
    for name in ('next','home','toggle','state'): sub.add_parser(name)
    args = parser.parse_args()
    try:
        token = os.environ.get('DESKDISPLAY_TOKEN') or args.token_file.read_text().strip()
        if args.command in ('next','home','toggle'):
            path, body = '/button', {'action': {'next':'short','home':'double','toggle':'long'}[args.command]}
        elif args.command == 'state': path, body = '/state', None
        else:
            path = {'brightness':'/display'}.get(args.command,'/'+args.command)
            key = {'mode':'mode','page':'page','brightness':'brightness','message':'text'}[args.command]
            value = 'loft' if (args.command, args.value) == ('page', 'clock') else args.value  # deprecated alias
            body = {key: value}
        request = Request(args.host.rstrip('/')+path, data=json.dumps(body).encode() if body is not None else None,
                          headers={'Authorization':'Bearer '+token,'Content-Type':'application/json'})
        with urlopen(request, timeout=5) as response: result = json.load(response)
        print(json.dumps(result, ensure_ascii=False, indent=2))
    except (OSError, ValueError, HTTPError, URLError) as exc:
        print(f'deskctl: {exc}',file=sys.stderr); return 1
    return 0


if __name__ == '__main__': sys.exit(main())
