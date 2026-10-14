<%
Option Explicit
Const Limit = 3
Dim values(), number, total, label, item
ReDim values(1)
values(0) = "A"
values(1) = "B"
ReDim Preserve values(2)
values(2) = "C"

total = 0
For number = 3 To 1 Step -1
    total = total + number
Next

Do Until total = 8
    total = total + 1
Loop

Do
    total = total + 1
Loop Until total = 9

If False Then
    label = "wrong"
ElseIf total = 9 Then
    label = "elseif"
Else
    label = "wrong"
End If

Select Case values(2)
Case "A", "B"
    label = "wrong"
Case "C"
    label = label & "-case"
Case Else
    label = "wrong"
End Select

For Each item In values
    label = label & item
Next

Response.Write CStr(Limit) & "|" & CStr(UBound(values)) & "|" & CStr(total) & "|" & label
Erase values
%>
