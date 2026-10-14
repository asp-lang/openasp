<%
Function StripGuestbook(ByRef value)
    Dim startPos
    Dim endPos
    Dim code
    Dim iterations
    startPos = InStr(value, "[QS_GUESTBOOK:")
    While startPos <> 0 And iterations < 3
        endPos = InStr(startPos, value, "]")
        code = Mid(value, startPos + 14, endPos - startPos - 14)
        value = Replace(value, "[QS_GUESTBOOK:" & code & "]", "", 1, -1, 1)
        startPos = InStr(value, "[QS_GUESTBOOK:")
        iterations = iterations + 1
    Wend
    StripGuestbook = value
End Function

Dim marker
marker = "before[QS_GUESTBOOK:SAMPLE]after"
Response.Write "result=" & StripGuestbook(marker) & vbCrLf
Response.Write "copyback=" & marker & vbCrLf
%>
