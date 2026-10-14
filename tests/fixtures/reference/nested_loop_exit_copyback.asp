<%
Class ExitProbe
    Public Sub Update(ByRef value)
        Dim outer, inner
        For Each outer In Array(1, 2)
            For Each inner In Array(1, 2)
                value = value + 10
                Exit Sub
            Next
        Next
        Response.Write "bad-sub|"
    End Sub

    Public Function Lookup(ByRef value)
        Dim index
        For index = 1 To 3
            value = value + 1
            Lookup = index
            Exit Function
        Next
        Response.Write "bad-function|"
    End Function

    Public Function Pick()
        Dim item
        For Each item In Array(2)
            Select Case item
                Case 1, 2
                    Pick = "selected"
                    Exit Function
            End Select
        Next
        Response.Write "bad-select|"
    End Function

    Public Function Run()
        Dim value
        value = 0
        Call Update(value)
        Response.Write "sub:" & value & "|"
        Call Lookup(value)
        Response.Write "for:" & value & "|"
        Response.Write Pick() & "|"
        Dim item, count
        count = 0
        For Each item In Array(1, 2, 3)
            count = count + 1
            Exit For
        Next
        Response.Write "break:" & count & "|"
        Run = True
    End Function
End Class

Function Outer()
    Dim probe
    Set probe = New ExitProbe
    Call probe.Run()
    Outer = True
    Response.Write "outer|"
End Function

Response.Write CStr(Outer()) & "|end"
%>
