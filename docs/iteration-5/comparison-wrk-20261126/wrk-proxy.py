import json,os,signal,socket,subprocess,time,sys,threading,resource,platform
from pathlib import Path
R=Path('/tmp/iter5-compare-20261125'); B=R/'jaeseong/build/test_backend'; W=R/'wrk-local/usr/bin/wrk'
L='/mnt/c/Users/David/SW-AI-W02-03-TEMPLATE/SW-Web-Server/iteration5_proxy_mix.lua'
env=os.environ.copy();env['LD_LIBRARY_PATH']=str(R/'wrk-local/usr/lib/x86_64-linux-gnu')
def freeport():
 s=socket.socket();s.bind(('127.0.0.1',0));p=s.getsockname()[1];s.close();return p
def stop(proc):
 if proc and proc.poll() is None:
  try:os.killpg(proc.pid,signal.SIGTERM);proc.wait(timeout=6)
  except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait()
def ready(p,proc):
 for i in range(200):
  if proc.poll() is not None:raise RuntimeError(f'process exit {proc.returncode}')
  try:s=socket.create_connection(('127.0.0.1',p),.1);s.close();return
  except OSError:time.sleep(.025)
 raise RuntimeError('startup timeout')
def pids(pid):
 out=[pid]
 try:
  for p in Path(f'/proc/{pid}/task/{pid}/children').read_text().split():out+=pids(int(p))
 except OSError:pass
 return out
def sample(pid):
 cpu=rss=fds=0
 for p in pids(pid):
  try:
   st=Path(f'/proc/{p}/stat').read_text().split();cpu+=int(st[13])+int(st[14]);rss+=int(st[23])*os.sysconf('SC_PAGE_SIZE');fds+=len(list(Path(f'/proc/{p}/fd').iterdir()))
  except (OSError,IndexError):pass
 return cpu,rss,fds
results={'environment':{'platform':platform.platform(),'cpus':os.cpu_count(),'fd_limit':resource.getrlimit(resource.RLIMIT_NOFILE),'wrk':'4.1.0-3build1','duration_seconds':30,'repeats':3,'generator_threads':'1 when connections=1 else 4','server_workers':4,'model':'closed-loop, connection close, alternating 1KiB POST /api/submissions and GET /api/submission-status, verified response body/status/backend ID; C backends single threaded, all processes same WSL','timeout_seconds':5},'commits':{'jaeseong':'0723287dd773c1b0bc82199cfa74d91ed3054384','chan':'af48c913fedc934f16f248d73b78fdee430259ef','yyj':'c0e352778f6ca564bd7a6a90468790eb8b609658'},'results':{}}
backends=[];server=None
try:
 ports=[freeport() for _ in range(3)]
 for role,p in zip(('submission','query-a','query-b'),ports):
  proc=subprocess.Popen([str(B),str(p),role,'normal'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True);backends.append(proc);ready(p,proc)
 for branch in ('jaeseong','chan','yyj'):
  d=R/branch;port=freeport();cfg=R/(branch+'-wrk.conf');cfg.write_text(('worker-policy rr\nquery-policy rr\n' if branch=='yyj' else '')+''.join(f'{n} 127.0.0.1:{p}\n' for n,p in zip(('submission','query-a','query-b'),ports)))
  args=[str(d/'build/server'),str(port),str(cfg)]+(['4'] if branch!='yyj' else [])
  log=open(R/(branch+'-wrk-server.log'),'w');server=subprocess.Popen(args,cwd=d,stdout=log,stderr=log,start_new_session=True);ready(port,server)
  results['results'][branch]=[]
  for c in (1,10,100,500,1000):
   for rep in range(1,4):
    start=sample(server.pid);observations=[];done=threading.Event()
    def collect():
     while not done.wait(1):observations.append(sample(server.pid))
    t=threading.Thread(target=collect);t.start()
    try:
     command=[str(W),'-t',str(min(4,c)),'-c',str(c),'-d','30s','--timeout','5s','--latency','-s',L,f'http://127.0.0.1:{port}']
     run=subprocess.run(command,env=env,capture_output=True,text=True,timeout=45)
    finally:done.set();t.join()
    end=sample(server.pid);record={'connections':c,'repeat':rep,'command':command,'exit':run.returncode,'stdout':run.stdout,'stderr':run.stderr,'cpu_seconds':(end[0]-start[0])/os.sysconf('SC_CLK_TCK'),'peak_rss_sum_bytes':max([start[1],end[1]]+[x[1] for x in observations]),'peak_fd_sum':max([start[2],end[2]]+[x[2] for x in observations])}
    results['results'][branch].append(record);(R/'wrk-proxy-results.json').write_text(json.dumps(results,indent=2));print(branch,c,rep,'exit',run.returncode,run.stdout.split('VERIFIED')[-1].strip(),flush=True)
    if run.returncode:raise RuntimeError('wrk failure')
  stop(server);server=None;log.close()
finally:
 stop(server)
 for proc in backends:stop(proc)
