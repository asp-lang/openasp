<%
Set client = Server.CreateObject("OpenASP.Redis")
Call client.Open("Host=127.0.0.1;Port=" & Request("port") & ";Database=0;Timeout=2000")
Call client.Select(0)
Response.Write client.State
Response.Write ":"
Response.Write client.Set("component:string", "hello")
Response.Write ":"
Response.Write client.Get("component:string")
Response.Write ":"
Call client.Set("component:counter", "9")
Response.Write client.Incr("component:counter")
Response.Write ":"
Response.Write client.Decr("component:counter")
Response.Write ":"
Call client.SetEx("component:expiring", "short", 30)
Response.Write CStr(client.Exists("component:expiring"))
Call client.Expire("component:expiring", 30)
ttl = client.TTL("component:expiring")
If ttl > 0 Then Response.Write ":ttl"
Response.Write ":"
Response.Write client.HSet("component:hash", "name", "egret")
Response.Write ":"
Response.Write client.HGet("component:hash", "name")
Response.Write ":"
hashValues = client.HGetAll("component:hash")
Response.Write hashValues(0)
Response.Write "="
Response.Write hashValues(1)
Response.Write ":"
Call client.Del("component:list")
Response.Write client.LPush("component:list", "zero")
Response.Write ":"
Response.Write client.RPush("component:list", "one")
Response.Write ":"
Response.Write client.RPush("component:list", "two")
values = client.LRange("component:list", 0, -1)
Response.Write ":"
Response.Write values(0)
Response.Write ","
Response.Write values(1)
Response.Write ":"
Response.Write client.LPop("component:list")
Response.Write ":"
Response.Write client.RPop("component:list")
Response.Write ":"
Call client.Del("component:set")
Response.Write client.SAdd("component:set", "member")
setValues = client.SMembers("component:set")
Response.Write ":"
Response.Write setValues(0)
Response.Write ":"
Response.Write client.Execute("PING")
Call client.Del("component:string", "component:counter", "component:expiring", "component:hash", "component:list", "component:set")
Call client.Close()
Response.Write ":"
Response.Write client.State
%>
