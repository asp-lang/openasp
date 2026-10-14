<%
' Class routine locals must shadow globals in VM slots.
Class LocalLoop
    Public Names()
    Public Values()

    Private Sub Class_Initialize()
        ReDim Names(3)
        ReDim Values(3)
        Names(0) = "a"
        Names(1) = "b"
        Names(2) = "c"
        Names(3) = "d"
        Values(0) = "va"
        Values(1) = "vb"
        Values(2) = "vc"
        Values(3) = "vd"
    End Sub

    Public Function GetValue(name)
        Dim n, i
        i = 0
        For Each n In Names
            If n = name Then
                GetValue = Values(i)
                Exit Function
            End If
            i = i + 1
        Next
    End Function
End Class

Dim i, item
i = 44
Set item = New LocalLoop
Response.Write item.GetValue("d") & "," & i
%>
