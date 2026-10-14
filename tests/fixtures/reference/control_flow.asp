<%
Function Bump(ByRef value)
    value = value + 1
    Bump = value
End Function

Class Counter
    Dim lastValue

    Function AddOne(ByRef input)
        input = input + 1
        lastValue = input
        AddOne = lastValue
    End Function
End Class

Dim total
Dim picked
Dim i
Dim pieces
Dim list
Dim idx
Dim n

total = 0
For i = 1 To 3
    total = total + i
Next

idx = 0
While idx < 2
    idx = idx + 1
Wend

n = 0
Do While n < 2
    n = n + 1
Loop

list = Array("a", "b")
For Each picked In list
    total = total + 1
Next

ReDim Preserve list(3)
list(2) = "c"
list(3) = "d"

Select Case total
Case 9
    pieces = "ok"
Case Else
    pieces = "bad"
End Select

Set counter = New Counter
pieces = pieces & ":" & CStr(Bump(total)) & ":" & CStr(total) & ":" & CStr(counter.AddOne(total)) & ":" & CStr(total) & ":" & list(3)
Response.Write pieces
%>
