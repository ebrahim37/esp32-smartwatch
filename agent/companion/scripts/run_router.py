#!/usr/bin/python3
"""Logged finite autorouter run: success file, crash, exit and wall-time gates."""
import pathlib,subprocess,time,selectors,sys,json,shutil
ROOT=pathlib.Path(__file__).resolve().parents[1]
repair='--repair' in sys.argv;revision='--revision' in sys.argv;name='revision' if revision else 'repair' if repair else 'companion'
out=ROOT/('routing/'+name+'.ses');logpath=ROOT/('routing/freerouting-'+name+'.log')
version='2.1.0' if repair else '2.5.0'
jar=ROOT/'tools'/('freerouting-'+version+'.jar')
if not jar.is_file():
    raise SystemExit(f'Missing FreeRouting {version}: install the official freerouting-{version}.jar at {jar}. No router has been run.')
java=shutil.which('java')
if java is None:
    raise SystemExit('Missing Java runtime: install Java compatible with the selected FreeRouting release and put java on PATH.')
dsn=ROOT/('routing/'+name+'.dsn')
if not dsn.is_file():
    raise SystemExit(f'Missing routing input: {dsn}. Export the matching companion DSN before running the router.')
cmd=[java,'-Xmx1024m','-Xss8m','-jar',str(jar),'--gui.enabled=false','--user_data_path='+str(ROOT/('routing/settings-'+name)),'-da','--profile.allow_telemetry=false','--profile.allow_contact=false','-de',str(dsn),'-do',str(out),'-mp','60','-mt','0']
if repair:cmd.extend(['--router.fanout.enabled=false','-inc','POWER_HIGH,POWER_3V3,RF_50R'])
else:cmd.append('--router.layers.routable=true,false,true,true')
print(' '.join(cmd),flush=True)
start=time.monotonic();last=start;reason=None
with logpath.open('w') as log:
    log.write('COMMAND '+repr(cmd)+'\n');log.flush()
    proc=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1,cwd=ROOT/'routing')
    sel=selectors.DefaultSelector();sel.register(proc.stdout,selectors.EVENT_READ)
    while proc.poll() is None:
        for key,_ in sel.select(timeout=1):
            line=key.fileobj.readline()
            if line:
                log.write(line);log.flush();print(line,end='',flush=True);last=time.monotonic()
                if any(x in line for x in ('OutOfMemoryError','StackOverflowError','FATAL')): reason='fatal crash signature'
        if time.monotonic()-start>900:reason='900s wall-clock timeout'
        if time.monotonic()-last>180:reason='180s without output/progress'
        if reason:proc.terminate();break
    try:proc.wait(timeout=15)
    except subprocess.TimeoutExpired:proc.kill();proc.wait()
    rest=proc.stdout.read();log.write(rest);print(rest,end='',flush=True)
result=dict(command=cmd,returncode=proc.returncode,elapsed_s=time.monotonic()-start,termination=reason,session_written=out.exists() and out.stat().st_size>0)
(ROOT/('routing/result-'+name+'.json')).write_text(json.dumps(result,indent=2));print(result,flush=True)
if reason or proc.returncode or not result['session_written']:sys.exit(1)
