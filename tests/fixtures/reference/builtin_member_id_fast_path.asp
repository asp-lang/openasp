<%
Session("member-id") = "session"
Application("member-id") = "application"
Response.Write Server.HTMLEncode(Request.QueryString("value"))
Response.Write "|" & Session("member-id") & "|" & Application("member-id")
%>
