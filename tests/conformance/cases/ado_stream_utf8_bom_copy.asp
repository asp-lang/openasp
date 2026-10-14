<%
Dim source, target
Set source = Server.CreateObject("ADODB.Stream")
Set target = Server.CreateObject("ADODB.Stream")
source.Type = 2
source.Open
source.WriteText "<meta>"
source.Position = 3
target.Type = 1
target.Open
source.CopyTo target
target.Position = 0
Response.Write target.Read
%>
