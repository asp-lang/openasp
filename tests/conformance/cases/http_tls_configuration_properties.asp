<%
Dim http

Set http = Server.CreateObject("OpenASP.HttpClient")
http.Timeout = 6789
http.MaxResponseBytes = 54321
http.TLSVerify = False
http.CAFile = "/tmp/test-ca.pem"
Response.Write CStr(http.Timeout)
Response.Write "|" & CStr(http.MaxResponseBytes)
Response.Write "|" & CStr(http.TLSVerify)
Response.Write "|" & http.CAFile
%>
