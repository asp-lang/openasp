<%
On Error Resume Next
Set shell = Server.CreateObject("Shell.Application")
Call shell.ShellExecute("/usr/bin/touch", """/tmp/egret-asp-shell-enabled""", "/tmp", "open", 0)
Response.Write CStr(Err.Number) & "|" & Err.Description
%>
