<%
Dim multi
Dim cacheA
Dim cacheB
Dim cacheFailed
Dim first
Dim second
Dim stored
Dim missingDelete
Dim counter
Dim touched
Dim version
Dim flushed
Dim serverError
Dim failed

Set multi = Server.CreateObject("OpenASP.MemcacheMulti")

Set cacheA = multi.CreateClient("Host=127.0.0.1;Port=" & Request("port_a") & ";Timeout=2000")
Set cacheB = multi.CreateClient("Host=127.0.0.1;Port=" & Request("port_b") & ";Protocol=text;TLS=off;Timeout=2000")
Set cacheFailed = multi.CreateClient("Host=127.0.0.1;Port=" & Request("failed_port") & ";Timeout=2000")

Set first = cacheA.CreateCommand("get", "first")
Set second = cacheA.CreateCommand("gets", "second")
Set stored = cacheA.CreateCommand("set", "stored", "payload", 3, 60)
Set missingDelete = cacheA.CreateCommand("delete", "missing")

Set counter = cacheB.CreateCommand("incr", "counter", 2)
Set touched = cacheB.CreateCommand("touch", "item", 30)
Set version = cacheB.CreateCommand("version")
Set flushed = cacheB.CreateCommand("flush_all")
Set serverError = cacheB.CreateCommand("get", "broken")

Set failed = cacheFailed.CreateCommand("get", "unreachable")

multi.MaxConcurrency = 2
multi.Timeout = 5000
multi.MaxRequestBytes = 4194304
multi.MaxResponseBytes = 16777216
multi.Execute

Response.Write CStr(multi.ClientCount) & ":" & CStr(multi.Count) & ":" & CStr(multi.MaxConcurrency)
Response.Write ":" & CStr(multi.Timeout) & ":" & CStr(multi.MaxRequestBytes) & ":" & CStr(multi.MaxResponseBytes)
Response.Write "|" & first.Value & ":" & CStr(first.Flags) & ":" & CStr(first.Cas)
Response.Write "|" & second.Value & ":" & CStr(second.Flags) & ":" & CStr(second.Cas)
Response.Write "|" & CStr(stored.Value) & ":" & CStr(missingDelete.Value)
Response.Write "|" & CStr(counter.Value) & ":" & CStr(touched.Value) & ":" & version.Value & ":" & CStr(flushed.Value)
Response.Write "|" & CStr(serverError.ErrorCode) & ":" & serverError.ErrorMessage
Response.Write "|" & CStr(failed.Completed) & ":" & CStr(failed.ErrorCode <> 0) & ":" & CStr(failed.MaybeExecuted)
%>
