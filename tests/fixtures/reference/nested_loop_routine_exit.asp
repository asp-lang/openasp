<%
Class Probe
    Public Function Find()
        Dim item
        For Each item In Array("match")
            If item = "match" Then
                Find = True
                Exit Function
            End If
        Next
        Response.Write "unreachable|"
    End Function

    Public Function Save()
        Call Find()
        Response.Write "saved|"
        Save = True
    End Function
End Class

Function DeleteProbe()
    Dim target
    Set target = New Probe
    Call target.Save()
    Response.Write "deleted|"
    DeleteProbe = True
End Function

Response.Write CStr(DeleteProbe()) & "|end"
%>
