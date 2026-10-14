<%
Dim http
Dim body

Set http = Server.CreateObject("OpenASP.HttpClient")
body = http.Get("http://127.0.0.1:" & Request("port") & "/status")
Response.Write CStr(http.Status) & "|" & CStr(http.ReadyState) & "|"
Response.Write http.ResponseText & "|" & http.ContentType
%>
