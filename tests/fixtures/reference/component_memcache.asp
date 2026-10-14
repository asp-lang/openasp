<%
Dim cache
Dim stats
Dim port

Set cache = Server.CreateObject("OpenASP.Memcache")
cache.Host = "127.0.0.1"
port = CInt(Request("port"))
cache.Port = port
cache.Timeout = 5000
cache.Protocol = "text"
Response.Write cache.Host & "|" & CStr(cache.Port)
Response.Write "|" & cache.Protocol & "|" & CStr(cache.TLSVerify)

Call cache.Open()
Response.Write "|" & CStr(cache.State)
Response.Write "|" & CStr(cache.Set("greeting", "hello", 7, 60))
Response.Write "|" & cache.Get("greeting")
Response.Write "|" & CStr(cache.LastFlags)
Response.Write "|" & cache.Gets("greeting")
Response.Write "|" & CStr(cache.LastCAS)
Response.Write "|" & CStr(cache.Append("greeting", "!"))
Response.Write "|" & CStr(cache.Prepend("greeting", ">"))
Response.Write "|" & cache.Get("greeting")
Response.Write "|" & CStr(cache.Add("added", "yes"))
Response.Write "|" & CStr(cache.Replace("added", "replaced"))
Response.Write "|" & CStr(cache.Set("counter", "5"))
Response.Write "|" & CStr(cache.Incr("counter", 3))
Response.Write "|" & CStr(cache.Decr("counter", 2))
Response.Write "|" & CStr(cache.Touch("greeting", 120))
Response.Write "|" & cache.Version()
Set stats = cache.Stats()
Response.Write "|" & stats("curr_items")
Response.Write "|" & CStr(cache.Delete("greeting"))
Response.Write "|" & CStr(IsNull(cache.Get("greeting")))
Response.Write "|" & CStr(cache.FlushAll())
Call cache.Close()
Response.Write "|" & CStr(cache.State)
%>
