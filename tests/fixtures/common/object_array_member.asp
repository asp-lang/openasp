<%
Class Metadata
    Public Names()

    Private Sub Class_Initialize()
        ReDim Names(1)
        Names(0) = "zero"
        Names(1) = "one"
    End Sub
End Class

Dim metadata
Set metadata = New Metadata
Response.Write metadata.Names(1)
%>
