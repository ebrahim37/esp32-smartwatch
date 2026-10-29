#!/usr/bin/python3
"""Run direct open-source routing; never relax KiCad rules/severities or swap pins.
Every invocation, stdout/stderr, duration and return code are retained. KiCad
CLI DRC, not this router's summary, is the final authority.
"""
import pathlib,subprocess,sys,json,time,shutil
ROOT=pathlib.Path(__file__).resolve().parents[1];TOOL=ROOT/'tools/KiCadRoutingTools';BOARD=ROOT.parents[1] / 'kicad/companion/companion.kicad_pcb';LOGS=ROOT/'routing/revision';LOGS.mkdir(exist_ok=True)
stage=sys.argv[1];extra=sys.argv[2:]
history=LOGS/'attempts'/str(time.time_ns());history.mkdir(parents=True)
for previous in (LOGS/(stage+'.log'),LOGS/(stage+'-run.json'),LOGS/(stage+'-summary.json')):
    if previous.exists():shutil.copy2(previous,history/previous.name)
common=['--no-fix-drc-settings','--escalation','off','--fab-tier','standard','--via-size','0.6','--via-drill','0.3','--board-edge-clearance','0.2','--hole-to-hole-clearance','0.254']
if stage=='planes':
    args=['py_router/route_planes.py',str(BOARD),'--overwrite','--nets','GND','GND','GND','--plane-layers','In1.Cu','F.Cu','B.Cu','--skip-existing-zones','--zone-clearance','.25','--stitch-vias','--stitch-pitch','5','--stitch-edge-fence','--stitch-fence-pitch','4']+common
elif stage=='power':
    names=['/Power/LTE_SW1','/Power/LTE_SW2','Net-(C4-Pad1)','Net-(U4-L1)','Net-(U4-L2)','/Power/USB_5V','/Power/VBAT','/Power/VSYS','/Modem/MODEM_3V8','/ESP32/SYS_3V3']
    widths=['1.2','1.2','1','0.7','0.7','1','1.2','1.2','1.2','0.5']
    args=['py_router/route.py',str(BOARD),'--overwrite','--nets',*names,'--power-nets',*names,'--power-nets-widths',*widths,'--layers','F.Cu','In2.Cu','B.Cu','--grid-step','.05','--max-ripup','20','--json-out',str(LOGS/'power-summary.json')]+common
elif stage=='usb':
    args=['py_router/route_diff.py',str(BOARD),'--overwrite','--nets','/USB/USB_D*','--layers','F.Cu','--impedance','90','--coplanar-gap','.25','--diff-pair-gap','.15','--diff-pair-intra-match','--length-match-tolerance','.5','--grid-step','.025','--json-out',str(LOGS/'usb-summary.json')]+common
elif stage=='signals':
    args=['py_router/route.py',str(BOARD),'--overwrite','--nets','*','!/Modem/*_ANT','!Net-(J_LTE1-SIN)','!Net-(J_GNSS1-SIN)','!Net-(C50-Pad1)','--layers','F.Cu','In2.Cu','B.Cu','--track-width','.15','--grid-step','.05','--max-ripup','40','--max-iterations','400000','--power-nets','GND','/Power/USB_5V','/Power/VBAT','/Power/VSYS','/Modem/MODEM_3V8','/ESP32/SYS_3V3','/Power/LTE_SW*','--power-nets-widths','.4','1','1.2','1.2','1.2','.5','1.2','--write-fill','--json-out',str(LOGS/'signals-summary.json')]+common
else:raise SystemExit('Expected planes, power, usb, or signals')
cmd=['/usr/bin/python3',str(TOOL/args[0]),*args[1:],*extra];start=time.time();print('Executing',cmd,flush=True)
with (LOGS/(stage+'.log')).open('w') as out:
    out.write('COMMAND '+json.dumps(cmd)+'\n');out.flush();result=subprocess.run(cmd,cwd=ROOT.parent,stdout=out,stderr=subprocess.STDOUT)
(LOGS/(stage+'-run.json')).write_text(json.dumps(dict(command=cmd,returncode=result.returncode,seconds=time.time()-start),indent=2))
print(stage,'exit',result.returncode,'seconds',round(time.time()-start,1),'log',str(LOGS/(stage+'.log')),flush=True)
sys.exit(result.returncode)
