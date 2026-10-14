<%
Class PropertyBox
    Private Fvalue

    Public Property Let Value(input)
        Fvalue = "set:" & input
    End Property

    Public Property Get Value
        Value = Fvalue
    End Property

    Public Function AssignInside(input)
        Value = input
        AssignInside = Value
    End Function
End Class

Dim box
Set box = New PropertyBox
box.Value = "external"
Response.Write box.Value & "|" & box.AssignInside("internal")
%>
