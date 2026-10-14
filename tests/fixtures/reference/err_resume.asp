<%
On Error Resume Next
Set dict = Server.CreateObject("Scripting.Dictionary")
missing = dict.NoSuchMember
Response.Write CStr(Err.Number)
Response.Write "|"
Response.Write Err.Source
Response.Write "|"
Err.Clear
Response.Write CStr(Err.Number)
On Error Goto 0
%>
