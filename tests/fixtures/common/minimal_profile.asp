<%
Set values = Server.CreateObject("Scripting.Dictionary")
values.CompareMode = 1
values.Add "Alpha", 40
values("beta") = 2
Response.Write values("ALPHA") + values("BETA")
%>
