<%
Dim ws
Dim port

port = Request("port")
Set ws = Server.CreateObject("OpenASP.WebSocket")
ws.Timeout = 5000
Call ws.Connect("ws://127.0.0.1:" & port & "/echo", "openasp.test")
Response.Write CStr(ws.State)
Response.Write "|" & CStr(ws.SendText("hello"))
Response.Write "|" & CStr(ws.SendBinary(ChrB(0) & ChrB(255)))
Response.Write "|" & ws.Receive()
Response.Write "|" & ws.MessageType
Response.Write "|" & CStr(ws.Opcode)
Response.Write "|" & CStr(ws.Final)
Response.Write "|" & CStr(ws.CloseCode)
Call ws.Close()
Response.Write "|" & CStr(ws.State)
%>
