<%
Set dict = Server.CreateObject("Scripting.Dictionary")
dict("color") = Request("color")
Session("palette") = dict
Session("list") = Array("zero", "one", "two")
Session.Timeout = 33

Application.Lock
Application("shared_list") = Session("list")
Application("shared_palette") = dict
Application.Unlock

Response.Write "stored"
%>
