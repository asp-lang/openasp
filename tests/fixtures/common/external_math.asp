<%
Set math = Server.CreateObject("Acme.Math")
Response.Write math.Version & ":" & math.Add(20, 22)
math.Scale = 2
Response.Write ":" & math.Add(20, 22)
%>
