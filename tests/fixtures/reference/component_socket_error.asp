<%
On Error Resume Next
Set socket = Server.CreateObject("OpenASP.Socket")
socket.Timeout = 2000
If Request("protocol") = "udp" Then
    Call socket.Bind("127.0.0.1", 0)
    Response.Write "bind=" & Err.Number & ":" & Err.Description & ";"
    Err.Clear
    sent = socket.SendTo("127.0.0.1", Request("port"), "socket-udp")
    Response.Write "send=" & sent & ":" & Err.Number & ":" & Err.Description
ElseIf Request("protocol") = "udpconnect" Then
    Call socket.Open("udp", "127.0.0.1", Request("port"))
    Response.Write "open=" & Err.Number & ":" & Err.Description & ";"
    Err.Clear
    sent = socket.Write("socket-client")
    Response.Write "write=" & sent & ":" & Err.Number & ":" & Err.Description & ";"
    Err.Clear
    data = socket.Read(1024)
    Response.Write "read=" & data & ":" & Err.Number & ":" & Err.Description
ElseIf Request("protocol") = "udpconnect2" Then
    socket.Protocol = "udp"
    socket.Host = "127.0.0.1"
    socket.Port = Request("port")
    Response.Write "config=" & socket.Protocol & ":" & socket.Host & ":" & socket.Port & ":" & socket.Timeout & ";"
    Call socket.Connect()
    Response.Write "open=" & Err.Number & ":" & Err.Description
Else
    Call socket.Open("tcp", "127.0.0.1", Request("port"))
    Response.Write "open=" & Err.Number & ":" & Err.Description & ";"
    Err.Clear
    sent = socket.Write("socket-client")
    Response.Write "write=" & sent & ":" & Err.Number & ":" & Err.Description & ";"
    Err.Clear
    data = socket.Read(1024)
    Response.Write "read=" & data & ":" & Err.Number & ":" & Err.Description
End If
Call socket.Close()
%>
