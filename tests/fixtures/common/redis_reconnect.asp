<%
Dim redis, reply
On Error Resume Next
Set redis = Server.CreateObject("OpenASP.Redis")
Call redis.Open("Host=127.0.0.1;Port=" & Request("redis_port") & ";Database=0;Timeout=1000")
reply = redis.Ping()
If Err.Number <> 0 Then
    Response.Write "error"
    Err.Clear
Else
    Response.Write reply
End If
Call redis.Close()
%>
