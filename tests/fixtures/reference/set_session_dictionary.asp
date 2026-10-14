<%
Set dict = Server.CreateObject("Scripting.Dictionary")
dict("color") = "blue"
Session("palette") = dict
Response.Write "stored"
%>
