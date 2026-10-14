<%
Dim redis, cache, values, keys

Set redis = Server.CreateObject("OpenASP.Redis")
Call redis.Open("Host=127.0.0.1;Port=" & Request("redis_port") & ";Database=0;Timeout=1000")
Response.Write redis.Ping()
Call redis.Close()

Set cache = Server.CreateObject("OpenASP.Memcache")
cache.Host = "127.0.0.1"
cache.Port = CInt(Request("memcache_port"))
cache.Timeout = 1000
Call cache.Open()
Call cache.Set("pool:a", "alpha")
Call cache.Set("pool:b", "beta")
keys = Array("pool:a", "pool:b", "pool:missing")
Set values = cache.GetMulti(keys)
Response.Write "|" & CStr(values.Count)
Response.Write "|" & values("pool:a")
Response.Write "|" & values("pool:b")
Response.Write "|" & CStr(values.Exists("pool:missing"))
Call cache.Close()
%>
