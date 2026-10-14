<!-- #include file="class_field_global_shadow.inc" -->
<%
Dim html
html = "global"

Dim renderer
Set renderer = New IncludedRenderer
Response.Write renderer.Build() & "|" & renderer.html & "|" & html
%>
