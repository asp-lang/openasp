<%
Class MemberFastPath
    Private stored

    Public Property Let Value(input)
        stored = input
    End Property

    Public Property Get Value
        Value = stored
    End Property

    Public Function Add(ByVal leftValue, ByVal rightValue)
        Add = leftValue + rightValue
    End Function

    Public Function ByRefFallback(ByRef input)
        input = input + 1
        ByRefFallback = input
    End Function
End Class

Dim item, number
Set item = New MemberFastPath
item.Value = "property"
number = 4
Response.Write item.Value & "|" & item.Add(2, 3) & "|" & item.ByRefFallback(number) & "|" & number
%>
<!-- object member id fixture -->
<!-- builtin metadata revision -->
