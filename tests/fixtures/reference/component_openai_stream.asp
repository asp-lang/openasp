<%
Dim ai
Dim stream
Dim mode

mode = Request("mode")
Set ai = Server.CreateObject("OpenASP.OpenAI")
ai.BaseURL = "http://127.0.0.1:" & Request("port") & "/v1"
ai.APIKey = "test-secret"
ai.Model = "mock-model"

If mode = "normal" Then
    Set stream = ai.OpenChatStream("hello")
    Do While stream.Read
        Response.Write stream.EventType & ":" & stream.Delta & ":" & stream.FinishReason & ":" & stream.Usage & "|"
    Loop
    Response.Write CStr(stream.Completed) & ":" & CStr(stream.ErrorCode) & ":" & CStr(stream.Status) & ":" & CStr(stream.ResponseBytes)
    Response.Write "|"
    Set stream = ai.OpenResponsesStream("{""model"":""mock-model"",""input"":""raw""}")
    Do While stream.Read
        Response.Write stream.EventType & ":" & stream.Delta & ":" & stream.Usage & "|"
    Loop
    Response.Write CStr(stream.Completed) & ":" & CStr(stream.ErrorCode) & ":" & CStr(stream.Status)
ElseIf mode = "limit" Then
    ai.MaxEventBytes = 32
    Set stream = ai.OpenStream("/oversize", "{}")
    Response.Write CStr(stream.Read) & ":" & CStr(stream.ErrorCode) & ":" & CStr(stream.Completed)
ElseIf mode = "http" Then
    Set stream = ai.OpenStream("/http-error", "{}")
    Response.Write CStr(stream.Read) & ":" & CStr(stream.ErrorCode) & ":" & CStr(stream.Status)
ElseIf mode = "idle" Then
    ai.IdleTimeout = 50
    Set stream = ai.OpenStream("/idle", "{}")
    Response.Write CStr(stream.Read) & ":" & CStr(stream.ErrorCode) & ":" & CStr(stream.Completed)
ElseIf mode = "close" Then
    Set stream = ai.OpenStream("/idle", "{}")
    stream.Close
    Response.Write CStr(stream.Completed) & ":" & CStr(stream.ErrorCode) & ":" & CStr(stream.Read)
End If
%>
