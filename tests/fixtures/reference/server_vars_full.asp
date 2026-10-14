<%
Response.Write Request.ServerVariables("gateway_interface")
Response.Write "|"
Response.Write Request.ServerVariables("server_name")
Response.Write "|"
Response.Write Request.ServerVariables("server_port")
Response.Write "|"
Response.Write Request.ServerVariables("document_root")
Response.Write "|"
Response.Write Request.ServerVariables("all_http")
%>
