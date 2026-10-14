<%
Dim cache

Set cache = Server.CreateObject("OpenASP.Memcache")
cache.Host = "example.test"
cache.Port = 12345
cache.Protocol = "text"
cache.Timeout = 6789
cache.TLS = True
cache.TLSVerify = False
Response.Write cache.Host
Response.Write "|" & CStr(cache.Port)
Response.Write "|" & cache.Protocol
Response.Write "|" & CStr(cache.Timeout)
Response.Write "|" & CStr(cache.TLS)
Response.Write "|" & CStr(cache.TLSVerify)
Response.Write "|" & CStr(cache.State)
Response.Write "|" & CStr(cache.LastFlags)
Response.Write "|" & CStr(cache.LastCAS)
%>
