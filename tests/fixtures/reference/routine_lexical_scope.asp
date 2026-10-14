<%
Dim sharedValue
sharedValue = "global"

Function ReadSharedValue()
    ReadSharedValue = sharedValue
End Function

Class ScopeProbe
    Public sharedValue

    Public Function ReadValue()
        ReadValue = ReadSharedValue()
    End Function
End Class

Set probe = New ScopeProbe
probe.sharedValue = "class"
Response.Write probe.ReadValue()
%>
