<%
Dim math, restored
Set math = Server.CreateObject("Acme.Math")
math.Scale = 7
Set Application("external-math") = math
Set restored = Application("external-math")
Response.Write restored(6) & vbCrLf
%>
