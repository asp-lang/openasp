<%
Option Explicit

Function UnsetResult()
    Exit Function
End Function

Response.Write CStr(Empty = False) & "|"
Response.Write CStr(False = Empty) & "|"
Response.Write CStr(Empty = 0) & "|"
Response.Write CStr(0 = Empty) & "|"
Response.Write CStr(Empty = "") & "|"
Response.Write CStr(Empty = True) & "|"
Response.Write CStr(Empty <> False) & "|"
Response.Write CStr(UnsetResult() = False) & "|"
If UnsetResult() = False Then
    Response.Write "not-verified"
Else
    Response.Write "verified"
End If
%>
