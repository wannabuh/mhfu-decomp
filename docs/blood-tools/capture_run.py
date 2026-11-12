# capture_run.py <name> <pictures: frames after the capture's, comma> [VAR=val ...]: load a ticket capture's state headless,
# plant hits with MHFU_WOUND_TEST, write pictures. CAP=<capture dir> (default: ticket 82's, a Tigrex in the desert); OUT=<dir for runs>
# (default /tmp/blood-runs); SWINGS=20:0 for no input; BIN=<binary>; T=<seconds>. Pictures land in OUT/run-<name>/build/pictures.
import os,sys,subprocess,shutil,glob,time
R=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))); S=os.environ.get('OUT','/tmp/blood-runs'); os.makedirs(S,exist_ok=True)
c=os.environ.get('CAP') or glob.glob(os.path.expanduser('~/.local/share/mhfu-recomp/tickets/0082-*/capture-0138'))[0]
name=sys.argv[1]; pics=[int(x) for x in sys.argv[2].split(',')]; over=dict(a.split('=',1) for a in sys.argv[3:])
F=int([l.split()[1] for l in open(c+'/info.txt') if l.startswith('frame ')][0])
d=f'{S}/run-{name}'; shutil.rmtree(d,ignore_errors=True); os.makedirs(d+'/build'); os.makedirs(d+'/st'); os.makedirs(d+'/cfg/mhfu-recomp')
os.symlink(R+'/game',d+'/game'); shutil.copy(c+'/state1.bin',d+'/st/state1.bin'); shutil.copytree(R+'/tests/data/saves/kit',d+'/SAVEDATA')
open(d+'/cfg/mhfu-recomp/settings.ini','w').write('iso = '+os.path.expanduser('~/Games/psp/mhfu.iso')+'\n')
lines=['mhfu-replay 1','epoch 1791983699225402','sens 0.150000006','camera 0','1 0 128 128 0 0']
script=os.environ.get('SWINGS','20:1000,110:1000,200:1000,290:1000,380:1000,470:1000,560:1000,650:1000')
for s in script.split(','):
    at,b=s.split(':'); at=int(at)
    stick=b.split('/')[1:] or ['128','128']
    lines.append(f'{F+at} {b.split("/")[0]} {stick[0]} {stick[1]} 0 0'); lines.append(f'{F+at+4} 0 128 128 0 0')
end=F+max(pics)+30
lines.append(f'end {end}')
open(d+'/in.rec','w').write('\n'.join(lines)+'\n')
env={k:v for k,v in os.environ.items() if not k.startswith('MHFU_')}
LEFT={"MHFU_REPLAY","MHFU_RECORD","MHFU_STATEDIR","MHFU_SAVEDIR","MHFU_CAPTUREDIR","MHFU_HEADLESS","MHFU_DUMP_AT","MHFU_DUMP_PRESENT","MHFU_STATE_LOAD","MHFU_STATE_SAVE","MHFU_TICKETDIR","MHFU_BLOOD_TRICKLES","MHFU_RESOLUTION","MHFU_FRAME_HISTORY"}
for l in open(c+'/env.txt'):
    if l.startswith('MHFU_') and '=' in l:
        k,v=l.rstrip('\n').split('=',1)
        if k not in LEFT: env[k]=v
env.update(XDG_CONFIG_HOME=d+'/cfg',XDG_DATA_HOME=d+'/data',MHFU_HEADLESS='1',MHFU_MUTE='1',MHFU_RENDERER='vulkan',MHFU_SAVEDIR=d+'/SAVEDATA',MHFU_STATEDIR=d+'/st',
  MHFU_STATE_LOAD='30,1',MHFU_PICTURES=','.join(str(F+p) for p in pics),MHFU_TAA='0',MHFU_NATIVE_SKIP='0',MHFU_SCALE='3',MHFU_REPLAY=d+'/in.rec',MHFU_WOUND_LOG='1',MHFU_MOTION_BLUR='0')
env.update(over)
log=open(d+'/log','w'); p=subprocess.Popen([os.environ.get('BIN',R+'/build/mhfu')],cwd=d,env=env,stdout=log,stderr=subprocess.STDOUT)
t=time.time()
while time.time()-t<float(os.environ.get('T','90')):
    if 'pictures done' in open(d+'/log',errors='replace').read() or p.poll() is not None: break
    time.sleep(0.5)
if p.poll() is None: p.kill()
print(sorted(os.path.basename(x) for x in glob.glob(d+'/build/pictures/*.png') if '-t' not in x), round(time.time()-t))
