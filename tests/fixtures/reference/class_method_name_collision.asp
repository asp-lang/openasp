<%
Dim globalRoute
globalRoute = ""

Function ResolveName(value)
    ResolveName = "global:" & value
End Function

Sub RecordRoute(value)
    globalRoute = "global-sub:" & value
End Sub

Class CollisionProbe
    Public result

    Private Sub Class_Initialize()
        result = ""
    End Sub

    Public Function ResolveName(value)
        ResolveName = "class:" & value
    End Function

    Public Sub RecordRoute(value)
        result = result & "|class-sub:" & value
    End Sub

    Public Function Run()
        result = ResolveName("expression")
        Call RecordRoute("statement")
        Run = result
    End Function
End Class

Dim probe
Set probe = New CollisionProbe
Response.Write probe.Run()
Response.Write "|"
Response.Write globalRoute
%>
