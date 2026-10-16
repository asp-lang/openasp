<%
Dim multi
Dim firstClient
Dim secondClient
Dim deleted
Dim pushed
Dim listValues
Dim stored
Dim loaded
Dim values

Set multi = Server.CreateObject("OpenASP.RedisMulti")
Set firstClient = multi.CreateClient("Host=127.0.0.1;Port=" & Request("port") & ";Database=14;Timeout=2000")
Set secondClient = multi.CreateClient("Host=127.0.0.1;Port=" & Request("port") & ";Database=14;Timeout=2000")

Set deleted = firstClient.CreateCommand("DEL", "redis-multi:list")
Set pushed = firstClient.CreateCommand("RPUSH", "redis-multi:list", "one", "two")
Set listValues = firstClient.CreateCommand("LRANGE", "redis-multi:list", 0, -1)

Set stored = secondClient.CreateCommand("SET", "redis-multi:stock", "42")
Set loaded = secondClient.CreateCommand("GET", "redis-multi:stock")

multi.MaxConcurrency = 2
multi.Execute
multi.Execute

values = listValues.Value
Response.Write CStr(deleted.ErrorCode) & ":" & CStr(pushed.Value)
Response.Write ":" & values(0) & "," & values(1)
Response.Write ":" & stored.Value & ":" & loaded.Value
%>
