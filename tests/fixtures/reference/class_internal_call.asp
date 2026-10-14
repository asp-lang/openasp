<%
Class Factory
    Private cached

    Private Sub Class_Initialize()
        Set cached = Nothing
    End Sub

    Public Function GetValue()
        If cached Is Nothing Then
            Set cached = Server.CreateObject("Scripting.Dictionary")
            cached.Add "name", "ok"
        End If
        Set GetValue = LoadValue()
    End Function

    Private Function LoadValue()
        Set LoadValue = cached
    End Function
End Class

Set factoryObject = New Factory
Set valueObject = factoryObject.GetValue()
Response.Write valueObject("name")
%>
