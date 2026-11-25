import http.client,json,os,signal,socket,subprocess,time,struct
from pathlib import Path
R=Path('/tmp/iter5-compare-20261125'); backend=R/'jaeseong/build/test_backend'
def port():
 s=socket.socket();s.bind(('127.0.0.1',0));p=s.getsockname()[1];s.close();return p
def req(p,path='/hello',body=None):
 c=http.client.HTTPConnection('127.0.0.1',p,timeout=6)
 try:
  c.request('POST' if body is not None else 'GET',path,body=body);r=c.getresponse();return {'status':r.status,'body':r.read().decode('latin1')}
 except Exception as e:return {'error':type(e).__name__,'message':str(e)[:120]}
 finally:c.close()
def start_backend(p,role,mode='normal'):
 proc=subprocess.Popen([str(backend),str(p),role,mode],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True);time.sleep(.08);assert proc.poll() is None;return proc
def stop(proc):
 if proc and proc.poll() is None:
  os.killpg(proc.pid,signal.SIGTERM)
  try:proc.wait(timeout=5)
  except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait()
def child(pid):
 try:return [int(p) for p in Path(f'/proc/{pid}/task/{pid}/children').read_text().split()]
 except OSError:return []
def fd_total(pid):
 try:return sum(len(list(Path(f'/proc/{p}/fd').iterdir())) for p in [pid]+child(pid))
 except OSError:return None
results={}
for name in ('jaeseong','chan','yyj'):
 d=R/name;ports=[port() for _ in range(4)];cfg=R/(name+'-fault.conf');cfg.write_text(('worker-policy rr\nquery-policy rr\n' if name=='yyj' else '')+''.join(f'{role} 127.0.0.1:{p}\n' for role,p in zip(('submission','query-a','query-b'),ports[1:])))
 args=[str(d/'build/server'),str(ports[0]),str(cfg)]+(['4'] if name!='yyj' else []);procs=[];server=None;one=None;out={}
 try:
  for role,p in zip(('submission','query-a','query-b'),ports[1:]):procs.append(start_backend(p,role))
  server=subprocess.Popen(args,cwd=d,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
  for _ in range(100):
   time.sleep(.02);check=req(ports[0]);
   if check.get('status')==200:break
  assert check.get('body')=='Hello';time.sleep(.2);out['initial_fds']=fd_total(server.pid)
  out['normal_submit']=req(ports[0],'/api/submissions',b'a\0b');out['normal_query']=[req(ports[0],'/api/hello') for _ in range(2)]
  stop(procs[0]);procs[0]=None;out['refused']=req(ports[0],'/api/submissions',b'a');out['alive_after_refused']=req(ports[0]);
  procs[0]=start_backend(ports[1],'submission','silent');out['silent']=req(ports[0],'/api/submissions',b'a');stop(procs[0]);procs[0]=None
  procs[0]=start_backend(ports[1],'submission','truncate');out['truncate']=req(ports[0],'/api/submissions',b'a');stop(procs[0]);procs[0]=None
  procs[0]=start_backend(ports[1],'submission');out['recovery']=req(ports[0],'/api/submissions',b'a');
  for _ in range(10):
   s=socket.create_connection(('127.0.0.1',ports[0]));s.sendall(b'GET /api/hello HTTP/1.1\r\nHost: x\r\n\r\n');s.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0));s.close()
  time.sleep(.5);out['alive_after_rst']=req(ports[0]);time.sleep(.5);out['after_fds']=fd_total(server.pid)
  if name=='yyj':
   workers=child(server.pid);assert len(workers)==4;os.kill(workers[0],signal.SIGKILL);time.sleep(.5);out['worker_kill']={'parent_alive':server.poll() is None,'children_before':len(workers),'children_after':len(child(server.pid)),'hello':req(ports[0])}
  else:out['worker_kill']='N/A: worker threads cannot be independently SIGKILLed'
  os.killpg(server.pid,signal.SIGTERM);server.wait(timeout=6);out['sigterm_exit']=server.returncode
 except Exception as e:out['harness_error']=repr(e)
 finally:
  stop(server)
  for proc in procs:stop(proc)
 results[name]=out;(R/'fault-results.json').write_text(json.dumps(results,indent=2));print(name,json.dumps(out),flush=True)
