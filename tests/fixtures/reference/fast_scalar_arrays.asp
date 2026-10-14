<%
Function SumArray(ByVal values, ByVal finish)
    Dim i, total
    total = 0
    For i = 0 To finish
        total = total + values(i)
    Next
    SumArray = total
End Function

Function FixedArray()
    Dim values(2)
    values(0) = 1
    values(1) = 2
    values(2) = 3
    FixedArray = SumArray(values, 2)
End Function

Function DynamicArray()
    Dim values()
    ReDim values(1)
    values(0) = 4
    values(1) = 5
    ReDim Preserve values(2)
    values(2) = 6
    DynamicArray = SumArray(values, 2)
End Function

Function Matrix()
    Dim values(1, 1)
    values(1, 0) = 9
    Matrix = values(1, 0)
End Function

Response.Write FixedArray() & ":" & DynamicArray() & ":" & Matrix()
%>
