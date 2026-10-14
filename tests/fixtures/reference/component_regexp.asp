<%
Set expression = Server.CreateObject("VBScript.RegExp")
expression.Pattern = "[a-z]+"
expression.Global = True
expression.IgnoreCase = True
Response.Write CStr(expression.Test("123 abc"))
Response.Write ":"
Set matches = expression.Execute("One 22 TWO")
Response.Write matches.Count
Response.Write ":"
Response.Write matches(0).Value
Response.Write ":"
Response.Write expression.Replace("One TWO", "x")
%>
