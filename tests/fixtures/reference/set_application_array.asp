<%
Session("list") = Array("zero", "one", "two")
Application.Lock
Application("shared_list") = Session("list")
Application.Unlock
Response.Write "stored"
%>
