<%
Dim expression
Set expression = New RegExp
expression.Pattern = "^[\.\_A-Za-z0-9\u4e00-\u9fa5]+$"
Response.Write CStr(expression.Test("cao"))
Response.Write "|"
Response.Write CStr(expression.Test(ChrW(20013) & ChrW(25991)))
Response.Write "|"
Response.Write CStr(expression.Test("bad name"))
%>
