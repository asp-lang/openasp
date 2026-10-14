<%
Set socket = Server.CreateObject("OpenASP.Socket")
socket.Timeout = 2000
Call socket.Bind("127.0.0.1", 0)
Response.Write socket.Protocol
Response.Write ":"
Response.Write socket.State
Response.Write ":"
Response.Write socket.LocalPort
Response.Write ":"
Response.Write socket.SendTo("127.0.0.1", Request("port"), "socket-udp")
packet = socket.ReceiveFrom(1024)
Response.Write ":"
Response.Write packet(0)
Response.Write ":"
Response.Write packet(1)
Response.Write ":"
Response.Write packet(2)
Call socket.Close()
Response.Write ":"
Response.Write socket.State
%>
