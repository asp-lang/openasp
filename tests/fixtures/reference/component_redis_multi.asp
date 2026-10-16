<%
Dim multi
Dim redisA
Dim redisB
Dim redisFailed
Dim ping
Dim echoValue
Dim wrongType
Dim stock
Dim ranking
Dim failed
Dim values

Set multi = Server.CreateObject("OpenASP.RedisMulti")

Set redisA = multi.CreateClient("Host=127.0.0.1;Port=" & Request("port_a") & ";Database=0;Timeout=2000")
Set redisB = multi.CreateClient("Host=127.0.0.1;Port=" & Request("port_b") & ";Database=0;Timeout=2000")
Set redisFailed = multi.CreateClient("Host=127.0.0.1;Port=" & Request("failed_port") & ";Database=0;Timeout=2000")

Set ping = redisA.CreateCommand("PING")
Set echoValue = redisA.CreateCommand("ECHO", "alpha")

Set wrongType = redisB.CreateCommand("HGET", "plain", "field")
Set stock = redisB.CreateCommand("GET", "stock")
Set ranking = redisB.CreateCommand("LRANGE", "ranking", 0, 1)

Set failed = redisFailed.CreateCommand("PING")

multi.MaxConcurrency = 2
multi.Timeout = 5000
multi.MaxRequestBytes = 4194304
multi.MaxResponseBytes = 16777216
multi.Execute

Response.Write CStr(multi.ClientCount) & ":" & CStr(multi.Count) & ":" & CStr(multi.MaxConcurrency)
Response.Write ":" & CStr(multi.Timeout) & ":" & CStr(multi.MaxRequestBytes) & ":" & CStr(multi.MaxResponseBytes)
Response.Write "|" & ping.Value & ":" & echoValue.Value
Response.Write "|" & CStr(wrongType.ErrorCode) & ":" & wrongType.ErrorMessage
Response.Write ":" & stock.Value
values = ranking.Value
Response.Write ":" & values(0) & "," & values(1)
Response.Write "|" & CStr(failed.Completed) & ":" & CStr(failed.ErrorCode <> 0) & ":" & CStr(failed.MaybeExecuted)
%>
