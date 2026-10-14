<%
Class MarkerObject
    Public Seen

    Public Function GetByCode(value)
        Seen = value
        GetByCode = (value = "SAMPLE")
    End Function

    Public Function Build()
        Build = "expanded-" & Seen
    End Function
End Class

Function ExpandGuestbook(ByRef value, fill)
    Dim startPos
    Dim endPos
    Dim code
    Dim item
    Dim iterations
    startPos = InStr(value, "[QS_GUESTBOOK:")
    While startPos <> 0 And iterations < 3
        endPos = InStr(startPos, value, "]")
        code = Mid(value, startPos + 14, endPos - startPos - 14)
        Set item = New MarkerObject
        If item.GetByCode(code) And fill Then
            value = Replace(value, "[QS_GUESTBOOK:" & code & "]", item.Build(), 1, -1, 1)
        Else
            value = Replace(value, "[QS_GUESTBOOK:" & code & "]", "", 1, -1, 1)
        End If
        Set item = Nothing
        startPos = InStr(value, "[QS_GUESTBOOK:")
        iterations = iterations + 1
    Wend
    ExpandGuestbook = iterations & ":" & value
End Function

Dim marker
marker = "before[QS_GUESTBOOK:SAMPLE]after"
Response.Write ExpandGuestbook(marker, True) & vbCrLf
Response.Write "copyback=" & marker & vbCrLf
%>
