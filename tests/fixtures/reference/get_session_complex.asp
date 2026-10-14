<%
Response.Write Session("palette")("color")
Response.Write ":"
Response.Write Session("list")(2)
Response.Write ":"
Response.Write CStr(Session.Timeout)
Response.Write ":"
Response.Write Application("shared_palette")("color")
Response.Write ":"
Response.Write Application("shared_list")(1)
%>
