<%
Const GLOBAL_OFFSET = 3

Function AddOffset(ByVal value)
    value = value + GLOBAL_OFFSET
    AddOffset = value
End Function

Function SumRange(ByVal finish)
    Dim i, total
    total = 0
    For i = 1 To finish
        total = total + AddOffset(i)
    Next
    SumRange = total
End Function

Response.Write AddOffset(4) & ":" & SumRange(3)
%>
