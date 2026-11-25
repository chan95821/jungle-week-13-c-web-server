import concurrent.futures,http.client,json,os,signal,socket,struct,subprocess,time
from pathlib import Path
R=Path('/tmp/iter5-compare-20261125'); BACKEND=R/'jaeseong/build/test_backend'
def port():
 with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
def stop(p):
 if p and p.poll() is None:
  os.killpg(p.pid,signal.SIGTERM)
  try:p.wait(timeout=5)
  except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
def start(p,role,mode='normal'):
 o=subprocess.Popen([str(BACKEND),str(p),role,mode],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True);time.sleep(.1);assert o.poll() is None;return o
def request(p,path='/hello',payload=None):
 c=http.client.HTTPConnection('127.0.0.1',p,timeout=7)
 try:
  c.request('POST' if payload is not None else 'GET',path,body=payload);r=c.getresponse();b=r.read();return {'status':r.status,'body':b.hex(),'backend':r.getheader('X-Backend-ID')}
 except Exception as e:return {'error':type(e).__name__}
 finally:c.close()
def raw(p,path):
 s=socket.create_connection(('127.0.0.1',p),2);s.settimeout(7);s.sendall(('POST '+path+' HTTP/1.1\r\nHost: t\r\nContent-Length: 1\r\nConnection: close\r\n\r\na').encode());s.shutdown(socket.SHUT_WR);data=b''
 try:
  while True:
   x=s.recv(16384)
   if not x:break
   data+=x
 except (ConnectionError,TimeoutError):pass
 finally:s.close()
 return {'prefix':data[:80].decode('latin1'),'status_lines':data.count(b'HTTP/1.1 '),'bytes':len(data)}
def children(pid):
 try:return list(map(int,Path(f'/proc/{pid}/task/{pid}/children').read_text().split()))
 except OSError:return []
def fds(pid):
 try:return sum(len(list(Path(f'/proc/{x}/fd').iterdir())) for x in [pid]+children(pid))
 except OSError:return None
allresults={}
for name in ('jaeseong','chan','yyj'):
 d=R/name;ports=[port() for _ in range(4)];cfg=R/(name+'-stability.conf');cfg.write_text(('worker-policy rr\nquery-policy rr\n' if name=='yyj' else '')+''.join(f'{n} 127.0.0.1:{p}\n' for n,p in zip(('submission','query-a','query-b'),ports[1:])))
 procs=[];server=None;res={}
 try:
  for role,p in zip(('submission','query-a','query-b'),ports[1:]):procs.append(start(p,role))
  server=subprocess.Popen([str(d/'build/server'),str(ports[0]),str(cfg)]+(['4'] if name!='yyj' else []),cwd=d,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
  for _ in range(100):
   if request(ports[0]).get('status')==200:break
   time.sleep(.02)
  assert request(ports[0]).get('body')=='48656c6c6f';time.sleep(.25);res['fd_before']=fds(server.pid)
  res['binary_submit']=[request(ports[0],'/api/submissions',bytes(range(256))*4) for _ in range(3)]
  res['binary_submit']=[{'status':x.get('status'),'backend':x.get('backend'),'body_matches':x.get('body')==(bytes(range(256))*4).hex()} for x in res['binary_submit']]
  res['distribution']=[request(ports[0],'/api/submission-status') for _ in range(10)]
  stop(procs[0]);procs[0]=None
  res['refused']=[request(ports[0],'/api/submissions',b'a') for _ in range(10)]
  procs[0]=start(ports[1],'submission','silent');res['silent']=[request(ports[0],'/api/submissions',b'a') for _ in range(3)];stop(procs[0]);procs[0]=None
  procs[0]=start(ports[1],'submission','truncate');res['truncate_wire']=[raw(ports[0],'/api/submissions') for _ in range(10)];stop(procs[0]);procs[0]=None
  procs[0]=start(ports[1],'submission');res['recovered']=[request(ports[0],'/api/submissions',b'a') for _ in range(3)]
  stop(procs[1]);procs[1]=start(ports[2],'query-a','delayed-header')
  with concurrent.futures.ThreadPoolExecutor(max_workers=5) as ex:
   slow=ex.submit(request,ports[0],'/api/submission-status');time.sleep(.2)
   fast=list(ex.map(lambda _:request(ports[0]),range(4)));res['slow_and_fast']={'fast':fast,'slow':slow.result()}
  stop(procs[1]);procs[1]=start(ports[2],'query-a')
  for i in range(30):
   s=socket.create_connection(('127.0.0.1',ports[0]),2);s.sendall(b'GET /api/submission-status HTTP/1.1\r\nHost: t\r\n\r\n');s.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0));s.close()
  time.sleep(1);res['after_rst']=request(ports[0]);res['fd_after']=fds(server.pid)
  if name=='yyj':
   ws=children(server.pid);res['worker_events']=[]
   for i in range(2):
    ws=children(server.pid);os.kill(ws[0],signal.SIGKILL);time.sleep(.3);res['worker_events'].append({'remaining':len(children(server.pid)),'hello':request(ports[0]),'submit':request(ports[0],'/api/submissions',b'a')})
  else:res['worker_events']='N/A: threads are not independently SIGKILLable'
  os.killpg(server.pid,signal.SIGTERM);server.wait(timeout=6);res['shutdown_exit']=server.returncode
 except Exception as e:res['harness_error']=repr(e)
 finally:
  stop(server)
  for p in procs:stop(p)
 allresults[name]=res;(R/'stability-repeat.json').write_text(json.dumps(allresults,indent=2));print(name,json.dumps({'refused':res.get('refused'),'silent':res.get('silent'),'fd':(res.get('fd_before'),res.get('fd_after')),'worker':res.get('worker_events'),'error':res.get('harness_error')}),flush=True)
