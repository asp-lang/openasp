<%@ CODEPAGE=65001 %>
<%
Response.ContentType = "image/gif"
Response.AddHeader "cache-ctrol", "no-cache"
Response.BinaryWrite "GIF89a"
%>
