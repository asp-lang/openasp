<%
Application.Lock
Application("shared_list") = Array("zero", "one", "two")
Application.Unlock
Response.Write "stored"
%>
