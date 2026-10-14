<%
On Error Resume Next
Response.Cookies("password") = "secret"
Response.Write "value:" & Err.Number & "|"
Err.Clear
Response.Cookies("password").Path = "/"
Response.Write "path:" & Err.Number
%>
