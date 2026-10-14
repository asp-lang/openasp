<%
Response.Cookies("chatroom") = "ready"
Response.Cookies("chatroom").Path = "/chat"
Response.Write "cookie-path-ready"
%>
