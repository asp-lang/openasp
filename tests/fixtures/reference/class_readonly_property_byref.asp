<%
Option Explicit
On Error Resume Next

Class SourceProbe
    Private storedValue

    Private Sub Class_Initialize()
        storedValue = "stable"
    End Sub

    Public Property Get Value
        Value = storedValue
    End Property
End Class

Class SinkProbe
    Public Function Accept(ByRef value)
        If False Then value = "changed"
        Accept = value
    End Function
End Class

Dim source
Dim sink
Set source = New SourceProbe
Set sink = New SinkProbe
Response.Write sink.Accept(source.Value) & "|" & CStr(Err.Number) & "|" & source.Value
%>
