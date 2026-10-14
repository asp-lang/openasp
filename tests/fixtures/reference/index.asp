<%@ LANGUAGE="VBScript" %>
<!--#include file="header.inc" -->
<%
Dim name
name = Request("name")
Session("last_name") = name
Response.Write "<div>site=" & Application("site_name") & "</div>"
Response.Write "<div>phase=" & Application("phase") & "</div>"
%>
<div>hello=<%= name %></div>
<div>query=<%= Request.QueryString("name") %></div>
<div>session=<%= Session("last_name") %></div>
