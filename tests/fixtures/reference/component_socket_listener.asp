<%
On Error Resume Next
protocol = Request("protocol")
Set listener = Server.CreateObject("OpenASP.Socket")
listener.Timeout = 10000
If protocol = "unix" Then
    listener.Protocol = "unix"
    Call listener.Listen(Request("path"), 8)
Else
    listener.Protocol = "tcp"
    Call listener.Listen("127.0.0.1", Request("port"), 8)
End If
If Err.Number <> 0 Then
    Response.Write "listen-error:" & Err.Number & ":" & Err.Description
    Response.End
End If
Set client = listener.Accept(60000)
If Err.Number <> 0 Then
    Response.Write "accept-error:" & Err.Number & ":" & Err.Description
    Response.End
End If
data = client.Read(1024)
Call client.Write("echo:" & data)
Response.Write listener.Protocol
Response.Write ":"
Response.Write listener.State
Response.Write ":"
Response.Write client.State
Response.Write ":"
Response.Write data
Call client.Close()
Call listener.Close()
Response.Write ":"
Response.Write client.State
Response.Write ":"
Response.Write listener.State
%>
