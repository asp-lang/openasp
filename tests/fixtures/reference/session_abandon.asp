<%
Session("gone") = "before"
Session.Abandon
Response.Write "abandoned"
%>
