<%
Option Explicit

Dim process, forkResult, role, marker
Set process = Server.CreateObject("OpenASP.Process")
forkResult = process.Fork()

If forkResult = 0 Then
    role = "child"
    marker = 100
Else
    role = "parent"
    marker = 200
End If

marker = marker + 1
Response.Write role & ":" & CStr(marker) & vbCrLf
%>
