<%
Dim multi
Dim cache
Dim stored
Dim fetched
Dim seeded
Dim incremented
Dim version

Set multi = Server.CreateObject("OpenASP.MemcacheMulti")
Set cache = multi.CreateClient("Host=127.0.0.1;Port=" & Request("port") & ";Timeout=2000")

Set stored = cache.CreateCommand("set", "multi:value", "payload", 7, 60)
Set fetched = cache.CreateCommand("gets", "multi:value")
Set seeded = cache.CreateCommand("set", "multi:counter", "40")
Set incremented = cache.CreateCommand("incr", "multi:counter", 2)
Set version = cache.CreateCommand("version")

multi.Timeout = 5000
multi.Execute
Response.Write CStr(stored.Value) & ":" & fetched.Value & ":" & CStr(fetched.Flags)
Response.Write ":" & CStr(fetched.Cas > 0) & ":" & CStr(seeded.Value) & ":" & CStr(incremented.Value)
Response.Write ":" & CStr(Len(version.Value) > 0)

multi.Execute
Response.Write "|" & CStr(stored.Value) & ":" & fetched.Value & ":" & CStr(fetched.Flags)
Response.Write ":" & CStr(fetched.Cas > 0) & ":" & CStr(incremented.Value)
%>
