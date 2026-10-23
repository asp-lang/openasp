<%
Dim math
Set math = Server.CreateObject("Acme.Math")
Response.Write (math.Deadline() > 0) & vbCrLf
%>
