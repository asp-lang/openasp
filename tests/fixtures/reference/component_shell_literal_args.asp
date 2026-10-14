<%
On Error Resume Next
Set shell = Server.CreateObject("Shell.Application")
Call shell.ShellExecute("/usr/bin/touch", """egret-shell-literal;touch egret-shell-injected""", "/tmp", "open", 0)
Response.Write CStr(Err.Number) & "|" & Err.Description
%>
