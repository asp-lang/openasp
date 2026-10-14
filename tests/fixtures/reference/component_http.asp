<%
Dim http
Set http = Server.CreateObject("MSXML2.ServerXMLHTTP.6.0")
Call http.open("GET", Request("url"), False)
Call http.setRequestHeader("X-Component-Test", "yes")
Call http.send()
Response.Write http.readyState
Response.Write ":"
Response.Write http.status
Response.Write ":"
Response.Write http.responseText
Response.Write ":"
Response.Write http.getResponseHeader("content-type")
%>
