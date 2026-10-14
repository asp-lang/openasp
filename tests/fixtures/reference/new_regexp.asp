<%
Dim matcher
Set matcher = New RegExp
matcher.Pattern = "^a.+z$"
matcher.Global = True
Response.Write matcher.Test("abcz")
%>
