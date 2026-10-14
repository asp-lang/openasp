<%
Dim redis
Set redis = Server.CreateObject("OpenASP.Redis")
Call redis.Open("Host=127.0.0.1;Port=" & Request("redis_port") & ";Database=0;Timeout=1000")
Response.Write redis.Execute("MULTI")
Call redis.Close()
%>
