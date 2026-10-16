<%
Dim multi
Dim first
Dim second
Dim failed

Set multi = Server.CreateObject("OpenASP.HttpMulti")
Set first = multi.CreateRequest("GET", "http://127.0.0.1:" & Request("port") & "/parallel/first")
Set second = multi.CreateRequest("POST", "http://127.0.0.1:" & Request("port") & "/parallel/second")
Set failed = multi.CreateRequest("GET", "http://127.0.0.1:1/unreachable")

second.Body = "{""id"":123}"
second.SetHeader "Content-Type", "application/json"

multi.MaxConcurrency = 2
multi.Timeout = 5000
multi.Execute

Response.Write CStr(multi.Count) & ":" & CStr(multi.MaxConcurrency) & ":" & CStr(multi.Timeout)
Response.Write "|" & CStr(first.Status) & ":" & CStr(first.Completed) & ":" & CStr(first.ErrorCode) & ":" & first.ResponseText
Response.Write "|" & CStr(second.Status) & ":" & CStr(second.Completed) & ":" & CStr(second.ErrorCode) & ":" & second.ResponseText
Response.Write "|" & CStr(failed.Completed) & ":" & CStr(failed.ErrorCode)
%>
