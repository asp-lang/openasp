<%
Response.Write "discarded-before-transfer"
Server.Transfer "server_transfer_target.asp"
Response.Write "unreachable-after-transfer"
%>
