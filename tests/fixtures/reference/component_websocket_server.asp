<%
Dim listener
Dim client
Dim ws
Dim message
Dim port

Set listener = Server.CreateObject("OpenASP.Socket")
listener.Timeout = 5000
port = CInt(Request("port"))
Call listener.Listen("127.0.0.1", port, 8)
Set client = listener.Accept(5000)

Set ws = Server.CreateObject("OpenASP.WebSocket")
ws.Timeout = 5000
Call ws.Accept(client, "openasp.server")
message = ws.Receive()
Call ws.SendText("reply:" & message)
Response.Write CStr(ws.State)
Response.Write "|" & ws.MessageType
Response.Write "|" & CStr(client.State)
Call ws.Close()
Call listener.Close()
%>
