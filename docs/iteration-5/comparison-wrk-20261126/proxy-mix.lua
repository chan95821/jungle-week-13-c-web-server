local threads = {}
local body = string.rep('0123456789abcdef',64)
local status_body = 'status' .. string.char(0) .. 'ready' .. string.char(255)
function setup(thread)
  table.insert(threads, thread)
end
function init(args)
  n=0; submit_ok=0; query_a=0; query_b=0; invalid=0
end
function request()
 n=n+1
 if n%2==1 then
   return wrk.format('POST','/api/submissions',{['Content-Type']='application/octet-stream',['Connection']='close'},body)
 end
 return wrk.format('GET','/api/submission-status',{['Connection']='close'})
end
function response(status,headers,payload)
 local id=''
 for key,value in pairs(headers) do if string.lower(key)=='x-backend-id' then id=value end end
 if status==201 and id=='submission' and payload==body then submit_ok=submit_ok+1
 elseif status==200 and id=='query-a' and payload==status_body then query_a=query_a+1
 elseif status==200 and id=='query-b' and payload==status_body then query_b=query_b+1
 else invalid=invalid+1 end
end
function done(summary,latency,requests)
 local s,a,b,bad=0,0,0,0
 for _,thread in ipairs(threads) do s=s+thread:get('submit_ok');a=a+thread:get('query_a');b=b+thread:get('query_b');bad=bad+thread:get('invalid') end
 io.write(string.format('VERIFIED submit=%d query_a=%d query_b=%d invalid=%d\n',s,a,b,bad))
end
