<%
Dim limitedRequest
Dim requestClient
Dim rejected
Dim requestError
Dim limitedResponse
Dim responseClient
Dim responseCommand

Set limitedRequest = Server.CreateObject("OpenASP.MemcacheMulti")
Set requestClient = limitedRequest.CreateClient("Host=127.0.0.1;Port=" & Request("port") & ";Timeout=1000")
limitedRequest.MaxRequestBytes = 4
On Error Resume Next
Set rejected = requestClient.CreateCommand("get", "value")
requestError = Err.Number
Err.Clear
On Error GoTo 0

Set limitedResponse = Server.CreateObject("OpenASP.MemcacheMulti")
Set responseClient = limitedResponse.CreateClient("Host=127.0.0.1;Port=" & Request("port") & ";Timeout=1000")
Set responseCommand = responseClient.CreateCommand("get", "value")
limitedResponse.MaxResponseBytes = 4
limitedResponse.Execute

Response.Write CStr(requestError) & ":" & CStr(responseCommand.Completed)
Response.Write ":" & CStr(responseCommand.ErrorCode <> 0) & ":" & CStr(responseCommand.MaybeExecuted)
%>
