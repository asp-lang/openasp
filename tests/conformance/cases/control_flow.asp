<%
Option Explicit
Const Label = "flow"
Dim values(), item, i, total, counter, selected
ReDim values(2)
values(0) = 2
values(1) = 3
values(2) = 4

total = 0
For i = 1 To 3
    total = total + i
Next

counter = 0
Do While counter < 2
    counter = counter + 1
Loop

While counter < 3
    counter = counter + 1
Wend

For Each item In values
    total = total + item
Next

Select Case total
Case 15
    selected = "matched"
Case Else
    selected = "wrong"
End Select

On Error Resume Next
total = total / 0
Response.Write Label & "|" & CStr(counter) & "|" & selected & "|" & CStr(Err.Number <> 0)
Err.Clear
On Error GoTo 0
Erase values
%>
