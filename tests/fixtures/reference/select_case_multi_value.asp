<%
Option Explicit
Dim m, calls
calls=0
Function DaysInMonth(y,m)
    Dim d
    Select Case m
        Case 1,3,5,7,8,10,12
            d=31
        Case 4,6,9,11
            d=30
        Case 2
            If (y Mod 400=0) Or ((y Mod 4=0) And (y Mod 100<>0)) Then
                d=29
            Else
                d=28
            End If
    End Select
    DaysInMonth=d
End Function
Function Probe(v)
    calls=calls+1
    Probe=v
End Function
Function Classify(v)
    Select Case Probe(v)
        Case Probe(1),Probe(2),Probe(3)
            Select Case v
                Case 1,2
                    Classify="small"
                Case Else
                    Classify="three"
            End Select
            Exit Function
        Case Else
            Classify="other"
    End Select
End Function
For m=1 To 12
    Response.Write DaysInMonth(2026,m) & ","
Next
Response.Write "|" & DaysInMonth(2000,2) & "," & DaysInMonth(1900,2) & "," & DaysInMonth(2024,2)
Response.Write "|" & Classify(2) & ":" & calls
calls=0
Response.Write "|" & Classify(3) & ":" & calls
calls=0
Response.Write "|" & Classify(9) & ":" & calls
Response.Write "|"
Select Case 100
    Case 1,2,3
        Response.Write "incorrect"
End Select
Response.Write "done"
%>
