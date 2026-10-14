<%
Class Counter
    Private stored

    Public Property Let Value(nextValue)
        stored = nextValue
    End Property

    Public Property Get Value()
        Value = stored
    End Property

    Public Function Add(ByVal amount)
        stored = stored + amount
        Add = stored
    End Function
End Class

Function Bump(ByRef value)
    value = value + 1
    Bump = value
End Function

Dim objectValue, number
number = 4
Set objectValue = New Counter
objectValue.Value = number
Response.Write Bump(number) & "|" & objectValue.Add(3) & "|" & number & "|" & objectValue.Value
%>
