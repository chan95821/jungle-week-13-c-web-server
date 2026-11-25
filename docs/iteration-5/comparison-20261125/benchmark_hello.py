import asyncio, json, os, signal, socket, subprocess, time, resource, platform
from pathlib import Path
ROOT=Path('/tmp/iter5-compare-20261125'); CONC=(1,10,100,500,1000); REPS=3; DURATION=30

def pids(pid):
    out=[pid]
    try:
        for p in Path(f'/proc/{pid}/task/{pid}/children').read_text().split(): out+=pids(int(p))
    except (OSError,ProcessLookupError): pass
    return out

def resources(pid):
    ticks=rss=fds=threads=0
    for p in pids(pid):
        try:
            st=Path(f'/proc/{p}/stat').read_text().split();ticks+=int(st[13])+int(st[14]);rss+=int(st[23])*os.sysconf('SC_PAGE_SIZE');threads+=int(st[19]);fds+=len(list(Path(f'/proc/{p}/fd').iterdir()))
        except (OSError,IndexError): pass
    return ticks,rss,fds,threads

def port():
    s=socket.socket();s.bind(('127.0.0.1',0));p=s.getsockname()[1];s.close();return p

async def req(p, path='/hello', body=None):
    start=time.monotonic(); reader=writer=None
    try:
        reader,writer=await asyncio.wait_for(asyncio.open_connection('127.0.0.1',p),5)
        method='GET' if body is None else 'POST';body=body or b''
        writer.write(f'{method} {path} HTTP/1.1\r\nHost: test\r\nConnection: close\r\nContent-Length: {len(body)}\r\n\r\n'.encode()+body)
        await writer.drain(); data=await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'),5)
        status=int(data.split(b' ',2)[1]);hdr={}
        for line in data.split(b'\r\n')[1:]:
            if b':' in line:
                k,v=line.split(b':',1);hdr[k.lower()]=v.strip()
        if b'content-length' in hdr: payload=await asyncio.wait_for(reader.readexactly(int(hdr[b'content-length'])),5)
        else: payload=await asyncio.wait_for(reader.read(),5)
        ok=status==200 and path=='/hello' and payload==b'Hello'
        return ok,(time.monotonic()-start)*1000,str(status)
    except Exception as e: return False,(time.monotonic()-start)*1000,type(e).__name__
    finally:
        if writer:
            writer.close()
            try: await writer.wait_closed()
            except Exception: pass

async def trial(p,c,pid):
    stop=time.monotonic()+DURATION;counts={};times=[];r0=resources(pid);peaks=list(r0)
    async def client():
        while time.monotonic()<stop:
            ok,ms,code=await req(p);counts[('ok:' if ok else 'fail:')+code]=counts.get(('ok:' if ok else 'fail:')+code,0)+1
            if ok:times.append(ms)
    async def sample():
        while time.monotonic()<stop:
            await asyncio.sleep(1);r=resources(pid)
            for i in range(1,4):peaks[i]=max(peaks[i],r[i])
    t=time.monotonic();await asyncio.gather(*(client() for _ in range(c)),sample());elapsed=time.monotonic()-t;r1=resources(pid);times.sort()
    def q(v): return times[min(len(times)-1,int((len(times)-1)*v))] if times else None
    return {'concurrency':c,'duration':elapsed,'success':len(times),'rps':len(times)/elapsed,'counts':counts,'p50_ms':q(.5),'p95_ms':q(.95),'p99_ms':q(.99),'cpu_seconds':(r1[0]-r0[0])/os.sysconf('SC_CLK_TCK'),'peak_rss_bytes':peaks[1],'peak_fds':peaks[2],'peak_threads':peaks[3]}

async def main():
    out={'environment':{'platform':platform.platform(),'cpu_count':os.cpu_count(),'fd_limit':resource.getrlimit(resource.RLIMIT_NOFILE),'method':'closed-loop GET /hello, one request per connection, 5s timeout, same host Python generator; p50/p95/p99 successful responses only'},'results':{}}
    for branch in ('jaeseong','chan','yyj'):
        directory=ROOT/branch; p=port(); cfg=ROOT/(branch+'.conf');prefix='worker-policy rr\nquery-policy rr\n' if branch=='yyj' else ''
        cfg.write_text(prefix+'submission 127.0.0.1:8001\nquery-a 127.0.0.1:8002\nquery-b 127.0.0.1:8003\n')
        args=[str(directory/'build/server'),str(p),str(cfg)]+(['4'] if branch!='yyj' else [])
        with open(ROOT/(branch+'-bench.log'),'w') as log:
            server=subprocess.Popen(args,cwd=directory,stdout=log,stderr=log,start_new_session=True)
            try:
                for i in range(200):
                    if server.poll() is not None: raise RuntimeError(f'{branch} exited {server.returncode}')
                    try:
                        r,w=await asyncio.open_connection('127.0.0.1',p);w.close();await w.wait_closed();break
                    except OSError: await asyncio.sleep(.02)
                else: raise RuntimeError('startup timeout')
                check=await req(p);assert check[0],check
                out['results'][branch]=[]
                for c in CONC:
                    for repeat in range(REPS):
                        result=await trial(p,c,server.pid);result['repeat']=repeat+1;out['results'][branch].append(result)
                        (ROOT/'hello-results.json').write_text(json.dumps(out,indent=2));print(branch,c,repeat+1,round(result['rps'],1),result['counts'],flush=True)
            finally:
                os.killpg(server.pid,signal.SIGTERM)
                try:server.wait(timeout=6)
                except subprocess.TimeoutExpired:os.killpg(server.pid,signal.SIGKILL);server.wait()
    (ROOT/'hello-results.json').write_text(json.dumps(out,indent=2))

asyncio.run(main())
