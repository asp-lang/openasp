<%
Class ChildObject
    Public Flag

    Private Sub Class_Initialize()
        Flag = "nested-ok"
    End Sub
End Class

Class ParentObject
    Public Function Child()
        Set Child = New ChildObject
    End Function
End Class

Dim parent
Set parent = New ParentObject
Response.Write parent.Child.Flag
%>
