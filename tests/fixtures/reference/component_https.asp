<%
On Error Resume Next

Dim http
Dim body
Dim port

Set http = Server.CreateObject("OpenASP.HttpClient")
http.Timeout = 5000
http.TLSVerify = True
http.CAFile = Request("ca")
port = Request("port")
body = http.Get("https://localhost:" & port & "/secure")
Response.Write CStr(http.Status) & "|" & body & "|" & CStr(http.TLSVerify) & "|" & CStr(Err.Number) & "|" & Err.Description
%>
