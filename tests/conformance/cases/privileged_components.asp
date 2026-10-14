<%
Dim process
Set process = Server.CreateObject("OpenASP.Process")
Response.Write CStr(IsObject(process)) & "|"

On Error Resume Next
Set process = Server.CreateObject("Shell.Application")
Response.Write Err.Number
Err.Clear
On Error GoTo 0
%>
