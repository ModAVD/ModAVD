"""Run MaaYYs tasks against one ModAVD instance (regression item helper).

Mirrors MaaPiCli's Runner::run (MaaFramework v5) with a hand-given ADB device, so no MaaToolkit device scan runs
(that scan walks every qemu-system process on the box, production included). ADB goes to the private server only:
ANDROID_ADB_SERVER_PORT is set for this process and inherited by every adb MaaFramework spawns.

    python -I -X utf8 maayys_run.py --run <MaaYYs package dir> --adb <adb.exe> --port 20095 --address emulator-20000
        --task 困28 --input 困28挑战次数.挑战次数=3 --deadline 900

Task options are turned into pipeline overrides the way the PI configurator does it: an input option substitutes its
inputs (defaults unless given with --input <option>.<input>=<value>), a select / switch option takes its default case
(or --case <option>=<case>). Prints one line per task and ends with ALL OK / FAILED (exit 0 / 1, 2 on setup errors).
"""
import argparse
import ctypes as C
import json
import os
import subprocess
import sys
import threading
import time

ap = argparse.ArgumentParser()
ap.add_argument('--run', required=True, help='MaaYYs package dir (interface.json, maafw\\, tasks\\)')
ap.add_argument('--adb', required=True)
ap.add_argument('--port', required=True)
ap.add_argument('--address', required=True)
ap.add_argument('--resource', default='官服3')
ap.add_argument('--task', action='append', required=True, help='task name as in tasks\\<name>.json; repeatable')
ap.add_argument('--input', action='append', default=[], help='<option>.<input>=<value>')
ap.add_argument('--case', action='append', default=[], help='<option>=<case name>')
ap.add_argument('--set', action='append', default=[],
                help='<node>.<field>=<value>, an extra pipeline override for every task (启动游戏.package=com.netease.onmyoji)')
ap.add_argument('--deadline', type=int, default=900, help='seconds for all tasks, then a stop is posted')
ap.add_argument('--log-dir', default=None)
a = ap.parse_args()
if a.port in ('5037', '20049', '20050'):
    sys.exit('refusing a production adb server port')

RUN = a.run
os.environ['ANDROID_ADB_SERVER_PORT'] = a.port
FW = os.path.join(RUN, 'maafw')   # package layout: agent.exe loads the DLLs from <cwd>\maafw too
os.add_dll_directory(FW)
fw = C.CDLL(os.path.join(FW, 'MaaFramework.dll'))
ag = C.CDLL(os.path.join(FW, 'MaaAgentClient.dll'))
P, I64, I32, U64, U8, S = C.c_void_p, C.c_int64, C.c_int32, C.c_uint64, C.c_uint8, C.c_char_p


def sig(lib, name, res, *args):
    f = getattr(lib, name)
    f.restype, f.argtypes = res, list(args)
    return f


MaaVersion = sig(fw, 'MaaVersion', S)
MaaGlobalSetOption = sig(fw, 'MaaGlobalSetOption', U8, I32, P, U64)
MaaAdbControllerCreate = sig(fw, 'MaaAdbControllerCreate', P, S, S, U64, U64, S, S)
MaaControllerSetOption = sig(fw, 'MaaControllerSetOption', U8, P, I32, P, U64)
MaaControllerPostConnection = sig(fw, 'MaaControllerPostConnection', I64, P)
MaaControllerWait = sig(fw, 'MaaControllerWait', I32, P, I64)
MaaResourceCreate = sig(fw, 'MaaResourceCreate', P)
MaaResourcePostBundle = sig(fw, 'MaaResourcePostBundle', I64, P, S)
MaaResourceWait = sig(fw, 'MaaResourceWait', I32, P, I64)
MaaTaskerCreate = sig(fw, 'MaaTaskerCreate', P)
MaaTaskerBindResource = sig(fw, 'MaaTaskerBindResource', U8, P, P)
MaaTaskerBindController = sig(fw, 'MaaTaskerBindController', U8, P, P)
MaaTaskerInited = sig(fw, 'MaaTaskerInited', U8, P)
MaaTaskerPostTask = sig(fw, 'MaaTaskerPostTask', I64, P, S, S)
MaaTaskerPostStop = sig(fw, 'MaaTaskerPostStop', I64, P)
MaaTaskerWait = sig(fw, 'MaaTaskerWait', I32, P, I64)
MaaStringBufferCreate = sig(fw, 'MaaStringBufferCreate', P)
MaaStringBufferGet = sig(fw, 'MaaStringBufferGet', S, P)
MaaStringBufferDestroy = sig(fw, 'MaaStringBufferDestroy', None, P)
MaaAgentClientCreateV2 = sig(ag, 'MaaAgentClientCreateV2', P, P)
MaaAgentClientBindResource = sig(ag, 'MaaAgentClientBindResource', U8, P, P)
MaaAgentClientIdentifier = sig(ag, 'MaaAgentClientIdentifier', U8, P, P)
MaaAgentClientConnect = sig(ag, 'MaaAgentClientConnect', U8, P)
MaaAgentClientDisconnect = sig(ag, 'MaaAgentClientDisconnect', U8, P)
MaaAgentClientDestroy = sig(ag, 'MaaAgentClientDestroy', None, P)

SUCCEEDED = 3000
CTRL_SHORT_SIDE = 2
GLOBAL_LOG_DIR = 1
GLOBAL_SAVE_ON_ERROR = 7
ALL = (1 << 64) - 1
SCREENCAP_DEFAULT = ALL & ~8 & ~16 & ~32   # minus RawByNetcat, MinicapDirect, MinicapStream
INPUT_DEFAULT = ALL & ~8                   # minus EmulatorExtras


def log(msg):
    print(time.strftime('%H:%M:%S'), msg, flush=True)


def substitute(obj, values):
    """Replace "{name}" strings in a pipeline override (an int-typed input becomes an int)."""
    if isinstance(obj, dict):
        return {k: substitute(v, values) for k, v in obj.items()}
    if isinstance(obj, list):
        return [substitute(v, values) for v in obj]
    if isinstance(obj, str) and obj.startswith('{') and obj.endswith('}') and obj[1:-1] in values:
        return values[obj[1:-1]]
    return obj


def merge(dst, src):
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            merge(dst[k], v)
        else:
            dst[k] = v


inputs = {}
for s in a.input:
    k, _, v = s.partition('=')
    opt, _, name = k.partition('.')
    inputs.setdefault(opt, {})[name] = v
cases = dict(s.split('=', 1) for s in a.case)


def find_task(name):
    """The task named `name` in any tasks\\*.json (the file name is not always the task name: 御魂.json holds 自动御魂)."""
    tasks_dir = os.path.join(RUN, 'tasks')
    for fn in sorted(os.listdir(tasks_dir)):
        if not fn.endswith('.json'):
            continue
        doc = json.load(open(os.path.join(tasks_dir, fn), encoding='utf-8'))
        for t in doc.get('task', []):
            if t.get('name') == name:
                return doc, t
    sys.exit(f'no task named {name} under {tasks_dir}')


def build_task(name):
    doc, task = find_task(name)
    override = {}

    def apply(opt_names):
        for opt_name in opt_names:
            opt = doc['option'][opt_name]
            if opt.get('type') == 'input':
                values = {}
                for inp in opt.get('inputs', []):
                    v = inputs.get(opt_name, {}).get(inp['name'], inp.get('default', ''))
                    values[inp['name']] = int(v) if inp.get('pipeline_type') == 'int' else v
                merge(override, substitute(opt.get('pipeline_override', {}), values))
            else:
                want = cases.get(opt_name, opt.get('default_case'))
                case = next((c for c in opt.get('cases', []) if c['name'] == want), None)
                if case is None and opt.get('cases'):
                    case = opt['cases'][0]
                if case:
                    merge(override, case.get('pipeline_override', {}))
                    # a case can bring its own sub-options (御魂挑战目标=八岐大蛇 -> 大蛇挑战层数)
                    apply(case.get('option', []))

    apply(task.get('option', []))
    for s in a.set:
        k, _, v = s.partition('=')
        node, _, field = k.partition('.')
        merge(override, {node: {field: v}})
    return task['entry'], override


iface = json.load(open(os.path.join(RUN, 'interface.json'), encoding='utf-8'))
controller = next(c for c in iface['controller'] if c['name'] == 'Android')
resource = next(r for r in iface['resource'] if r['name'] == a.resource)
TASKS = [(n,) + build_task(n) for n in a.task]
for label, entry, override in TASKS:
    log(f'task {label}: entry {entry}, override {json.dumps(override, ensure_ascii=False)}')

log_dir = a.log_dir or os.path.join(RUN, 'debug')
os.makedirs(log_dir, exist_ok=True)
b = log_dir.encode('utf-8')
MaaGlobalSetOption(GLOBAL_LOG_DIR, C.cast(C.c_char_p(b), P), len(b))
# a node that times out leaves its last screenshot in <log dir>\on_error
on_error = C.c_uint8(1)
if not MaaGlobalSetOption(GLOBAL_SAVE_ON_ERROR, C.cast(C.byref(on_error), P), 1):
    log('note: SaveOnError not accepted')
ver = MaaVersion().decode()
log(f'MaaFramework {ver}; adb server port {a.port}; address {a.address}')

ctrl = MaaAdbControllerCreate(a.adb.encode(), a.address.encode(), SCREENCAP_DEFAULT, INPUT_DEFAULT,
                              b'{}', os.path.join(FW, 'MaaAgentBinary').encode())
if not ctrl:
    log('controller create failed')
    sys.exit(2)
short = C.c_int32(720)
MaaControllerSetOption(ctrl, CTRL_SHORT_SIDE, C.byref(short), 4)
res = MaaResourceCreate()
tasker = MaaTaskerCreate()
cid = MaaControllerPostConnection(ctrl)
rid = 0
for p in resource['path']:
    rid = MaaResourcePostBundle(res, os.path.normpath(os.path.join(RUN, p)).encode('utf-8'))
MaaTaskerBindController(tasker, ctrl)
MaaTaskerBindResource(tasker, res)
if MaaControllerWait(ctrl, cid) != SUCCEEDED:
    log('controller connect failed')
    sys.exit(2)
if MaaResourceWait(res, rid) != SUCCEEDED:
    log('resource load failed')
    sys.exit(2)

# agent (custom recognitions / actions live in agent.exe)
agent = MaaAgentClientCreateV2(None)
MaaAgentClientBindResource(agent, res)
buf = MaaStringBufferCreate()
MaaAgentClientIdentifier(agent, buf)
socket_id = MaaStringBufferGet(buf).decode()
MaaStringBufferDestroy(buf)
env = dict(os.environ)
env.update({
    'PI_INTERFACE_VERSION': 'v2.5.0', 'PI_CLIENT_NAME': 'MaaPiCli', 'PI_CLIENT_VERSION': ver,
    'PI_CLIENT_LANGUAGE': 'zh_cn', 'PI_CLIENT_MAAFW_VERSION': ver, 'PI_VERSION': iface.get('version', ''),
    'PI_CONTROLLER': json.dumps(controller, ensure_ascii=False), 'PI_RESOURCE': json.dumps(resource, ensure_ascii=False),
})
agent_exe = os.path.normpath(os.path.join(RUN, iface['agent']['child_exec']))
agent_log = open(os.path.join(log_dir, 'agent-stdout.txt'), 'w', encoding='utf-8', errors='replace')
child = subprocess.Popen([agent_exe] + iface['agent'].get('child_args', []) + [socket_id], cwd=RUN, env=env,
                         stdout=agent_log, stderr=subprocess.STDOUT)
# MaaAgentClientConnect has no timeout of its own: if agent.exe dies it waits forever.
conn = {}
th = threading.Thread(target=lambda: conn.setdefault('ok', MaaAgentClientConnect(agent)), daemon=True)
th.start()
t_end = time.time() + 60
while th.is_alive() and time.time() < t_end:
    if child.poll() is not None:
        log(f'agent exited early (code {child.returncode}); see {log_dir}\\agent-stdout.txt')
        os._exit(2)
    time.sleep(0.5)
if th.is_alive() or not conn.get('ok'):
    log('agent connect failed or timed out')
    child.kill()
    os._exit(2)
log(f'agent connected (pid {child.pid})')
if not MaaTaskerInited(tasker):
    log('tasker not inited')
    sys.exit(2)

stopped = threading.Event()


def watchdog():
    if not stopped.wait(a.deadline):
        log(f'DEADLINE {a.deadline}s reached: posting stop')
        MaaTaskerPostStop(tasker)


threading.Thread(target=watchdog, daemon=True).start()
ok = True
for label, entry, override in TASKS:
    t0 = time.time()
    tid = MaaTaskerPostTask(tasker, entry.encode('utf-8'), json.dumps(override, ensure_ascii=False).encode('utf-8'))
    st = MaaTaskerWait(tasker, tid)
    log(f'task {label} (entry {entry}) -> {"SUCCEEDED" if st == SUCCEEDED else st} in {time.time() - t0:.0f}s')
    ok = ok and st == SUCCEEDED
    if st != SUCCEEDED:
        break
stopped.set()
MaaAgentClientDisconnect(agent)
MaaAgentClientDestroy(agent)
try:
    child.wait(timeout=10)
except subprocess.TimeoutExpired:
    child.kill()
log('ALL OK' if ok else 'FAILED')
sys.exit(0 if ok else 1)
