<%
On Error Resume Next
Set shell = CreateObject("Shell.Application")
Response.Write CStr(Err.Number) & "|" & Err.Source & "|" & Err.Description
%>
