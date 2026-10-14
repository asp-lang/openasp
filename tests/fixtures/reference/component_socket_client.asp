<%
protocol = Request("protocol")
Set socket = Server.CreateObject("OpenASP.Socket")
socket.Timeout = 2000
If protocol = "unix" Then
    Call socket.Open("unix", Request("path"))
Else
    If protocol = "udp" Then
        Call socket.Open("udp", "127.0.0.1", Request("port"))
    Else
        Call socket.Open("tcp", "127.0.0.1", Request("port"))
    End If
End If
Response.Write socket.Protocol
Response.Write ":"
Response.Write socket.State
Response.Write ":"
Response.Write socket.Write("socket-client")
Response.Write ":"
Response.Write socket.Read(1024)
Call socket.Close()
Response.Write ":"
Response.Write socket.State
%>
