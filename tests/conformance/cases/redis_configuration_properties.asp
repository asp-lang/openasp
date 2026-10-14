<%
Dim client
Dim port

port = Request("port")
Set client = Server.CreateObject("OpenASP.Redis")
client.Host = "127.0.0.1"
client.Port = CLng(port)
client.Database = 2
client.Timeout = 3210
Response.Write client.Host
Response.Write "|" & CStr(client.Port)
Response.Write "|" & CStr(client.Database)
Response.Write "|" & CStr(client.Timeout)
Response.Write "|" & CStr(client.State)
Call client.Open()
Response.Write "|" & CStr(client.State)
Call client.Close()
Response.Write "|" & CStr(client.State)
%>
