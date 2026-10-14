<%
Function EnCrypt(ByVal sIn)
    If IsNumeric(sIn) Then
        sIn = CLng(sIn) * 111
    End If
    sIn = CStr(sIn)
    Dim x, y, abfrom, abto
    EnCrypt = ""
    abfrom = ""
    For x = 0 To 25
        abfrom = abfrom & Chr(65 + x)
    Next
    For x = 0 To 25
        abfrom = abfrom & Chr(97 + x)
    Next
    For x = 0 To 9
        abfrom = abfrom & CStr(x)
    Next
    abto = Mid(abfrom, 14, Len(abfrom) - 13) & Left(abfrom, 13)
    For x = 1 To Len(sIn)
        y = InStr(abfrom, Mid(sIn, x, 1))
        If y = 0 Then
            EnCrypt = EnCrypt & Mid(sIn, x, 1)
        Else
            EnCrypt = EnCrypt & Mid(abto, y, 1)
        End If
    Next
End Function
Response.Write EnCrypt(73) & ":" & EnCrypt("a-0")
%>
