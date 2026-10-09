"""Helper that lets the Room EQ web page run roomeq.sh.

Called through JSON-RPC:  Addons.ExecuteAddon {"addonid": "script.roomeq",
                                               "params": ["cmd=measure"]}
                      or  params: ["settings", "MIC_CH=2", "RQ_MAXBOOST=0", ...]
Long commands run detached; the page follows data/state.json and data/report.json.
"""
import json
import os
import re
import subprocess
import sys

SCRIPT = os.environ.get('ROOMEQ_SCRIPT', '/storage/roomeq/roomeq.sh')
CONF_DIR = os.environ.get('ROOMEQ_DIR', '/storage/.config/roomeq')
CONF = os.path.join(CONF_DIR, 'roomeq.conf')
DATA = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    'webinterface.roomeq', 'data')

COMMANDS = {'setup', 'on', 'off', 'new', 'measure', 'undo', 'apply', 'reset'}
NUMERIC = {'MIC_CH', 'RQ_FMAX', 'RQ_BASS', 'RQ_MAXF', 'RQ_MAXBOOST', 'RQ_BOOST_FMIN'}
FORMATS = {'', 'S16_LE', 'S24_LE', 'S24_3LE', 'S32_LE'}


def write_data(name, obj):
    os.makedirs(DATA, exist_ok=True)
    with open(os.path.join(DATA, name), 'w') as f:
        json.dump(obj, f, ensure_ascii=False)


def read_conf():
    conf = {}
    try:
        for line in open(CONF):
            m = re.match(r'^\s*([A-Z_]+)=(.*)$', line)
            if m:
                conf[m.group(1)] = m.group(2).strip().strip('"\'')
    except OSError:
        pass
    return conf


def save_settings(pairs):
    conf = read_conf()
    for k, v in pairs:
        v = v.strip()
        if k in NUMERIC:
            if v and not re.fullmatch(r'-?\d+(\.\d+)?', v):
                continue
        elif k == 'CARD':
            if v and not re.fullmatch(r'[A-Za-z0-9_]+', v):
                continue
        elif k == 'FORMAT':
            if v not in FORMATS:
                continue
        elif k == 'RQ_MIC_CAL':
            if v and not re.fullmatch(r'/[A-Za-z0-9_./ -]+', v):
                continue
        else:
            continue
        if v == '':
            conf.pop(k, None)
        else:
            conf[k] = v
    os.makedirs(CONF_DIR, exist_ok=True)
    with open(CONF, 'w') as f:
        f.write('# kodi-roomeq settings (written by the web page)\n')
        for k in sorted(conf):
            f.write('%s="%s"\n' % (k, conf[k]))
    write_data('settings.json', conf)
    # card / format changes need a new ALSA config
    if any(k in ('CARD', 'FORMAT') for k, _ in pairs):
        subprocess.Popen([SCRIPT, 'setup'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)


def main(argv):
    args = [a for a in argv if a]
    if not args:
        return
    if args[0] == 'settings':
        save_settings([a.split('=', 1) for a in args[1:] if '=' in a])
        return
    if args[0] == 'status':
        out = subprocess.run([SCRIPT, 'status'], capture_output=True, text=True).stdout.strip()
        try:
            st = json.loads(out)
        except ValueError:
            st = {'error': out[:200]}
        st['settings'] = read_conf()
        write_data('status.json', st)
        return
    if args[0].startswith('cmd='):
        cmd = args[0][4:]
        if cmd in COMMANDS:
            subprocess.Popen([SCRIPT, cmd], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             start_new_session=True)


if __name__ == '__main__':
    main(sys.argv[1:])
