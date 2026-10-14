<%
Dim socket
Dim redis
Dim oldSocket
Dim oldRedis
Dim oldSocketError
Dim oldRedisError
Dim connection
Dim providerError

Set socket = Server.CreateObject("OpenASP.Socket")
Set redis = Server.CreateObject("OpenASP.Redis")

On Error Resume Next
Set oldSocket = Server.CreateObject("Egret.Socket")
oldSocketError = Err.Number
Err.Clear
Set oldRedis = Server.CreateObject("Egret.Redis")
oldRedisError = Err.Number
Err.Clear
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=OpenASP.MySQL;Server=127.0.0.1")
providerError = Err.Number
On Error GoTo 0

Response.Write CStr(IsObject(socket))
Response.Write "|" & CStr(IsObject(redis))
Response.Write "|" & CStr(oldSocketError)
Response.Write "|" & CStr(oldRedisError)
Response.Write "|" & CStr(providerError)
%>
