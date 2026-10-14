<%
Class ReceiverProbe
    Public prefix

    Private Sub Class_Initialize()
        prefix = "receiver"
    End Sub

    Public Sub Emit(value)
        Response.Write prefix & ":" & value
    End Sub
End Class

Function BuildProbe()
    Set BuildProbe = New ReceiverProbe
End Function

Dim probe
Set probe = New ReceiverProbe
Call probe.Emit("direct")
Response.Write "|"
Call BuildProbe().Emit("computed")
%>
