<%
Set dict = Server.CreateObject("Scripting.Dictionary")
dict("color") = "blue"
Session("palette") = dict
Application.Lock
Application("shared_palette") = dict
Application.Unlock
Response.Write "stored"
%>
