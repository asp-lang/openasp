<%
On Error Resume Next
Set shell = Server.CreateObject("Shell.Application")
Call shell.ShellExecute("/egret/not-a-real-executable")
Response.Write "launch=" & CStr(Err.Number) & ":" & Err.Description & vbCrLf
Err.Clear
Call shell.ShellExecute("/usr/bin/true", "", "", "runas", 0)
Response.Write "verb=" & CStr(Err.Number) & ":" & Err.Description
%>
