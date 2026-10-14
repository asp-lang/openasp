<%
Set expression = Server.CreateObject("VBScript.RegExp")
expression.Pattern = "([a-z]+)@([a-z]+\.[a-z]{2,3})"
expression.Global = True
expression.IgnoreCase = True
Response.Write expression.Replace("Mail A@B.com and c@d.org", "<$1|$2>")
Response.Write ":"
Set matches = expression.Execute("Mail A@B.com and c@d.org")
Response.Write matches.Count
Response.Write ":"
Response.Write matches(0).Value
Response.Write ":"
Response.Write matches(0).FirstIndex
Response.Write ":"
expression.Global = False
Response.Write expression.Replace("A@B.com c@d.org", "[$1]")
%>
