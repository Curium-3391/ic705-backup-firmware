#!/usr/bin/env python3
"""Read-only B5 checkpoint reader. Use a folder containing one test run only."""
import json
import sys
from pathlib import Path
EVENTS = {
 10:'Native self-test begin',11:'Native self-test passed',20:'SD self-test begin',21:'SD self-test passed',
 22:'Consume trigger begin',23:'Trigger consumed',80:'SHA256SUMS write/readback begin',81:'SHA256SUMS verified',
 82:'Manifest write/readback begin',83:'Manifest verified',84:'Completion receipt begin',85:'Completion receipt returned',
 90:'Final SD sync begin',91:'Final SD sync returned',92:'SD unmount about to begin',93:'SD unmount returned',
 94:'Core returned',95:'Restart gate passed; about to arm watchdog',96:'Completion error hold',
 97:'Error/non-disruptive report sync begin',98:'Report sync returned'}
TARGETS=['IC704','IC705','IC706','WLAN','IC553']
PHASES=['acquire/JEDEC begin','pass1 SD stream begin','pass1 file closed',
        'pass2 hardware read checkpoint','pass2 hardware read finished','double-read matched','target returned']
def event_label(event):
    if 100 <= event < 150:
        target, phase = divmod(event-100,10)
        if phase < len(PHASES): return TARGETS[target]+' '+PHASES[phase]
    return EVENTS.get(event,'Unknown event')
def inspect(directory):
    valid=[]; warnings=[]
    for name in ['RB5_PROGRESS_A.json','RB5_PROGRESS_B.json','RB5_POST_RESTART.json']:
        p=Path(directory)/name
        if not p.exists(): continue
        try:
            data=json.loads(p.read_text())
            assert data['build_id']=='RB3-DEDICATED-BACKUP-FINAL-J140-V5'
            assert all(type(data[k]) is int and data[k]>=0 for k in ['seq','event','offset','requested_mask'])
            data['file']=name;data['phase']=event_label(data['event']);valid.append(data)
        except (OSError,ValueError,KeyError,AssertionError) as exc:
            warnings.append(f'{name}: unreadable/incomplete checkpoint ({type(exc).__name__})')
    valid.sort(key=lambda x:(x['seq'],x.get('post_restart',False)))
    return {'latest':valid[-1] if valid else None,'checkpoints':valid,'warnings':warnings,
      'limits':['Use files from one run only; stale files from another run can have a larger sequence.',
                'A checkpoint marks a reached boundary. It cannot prove its own sync finished or the following operation started.',
                'Event 92 is the last permitted SD checkpoint before unmount. Later events need a retained post-restart export.',
                'A battery pull may lose even recently written SD metadata; missing records do not identify an exact instruction.']}
if __name__=='__main__':
    result=inspect(sys.argv[1]);print(json.dumps(result,indent=2));sys.exit(0 if result['latest'] else 1)
