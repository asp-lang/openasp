<%
Class FieldArrayHolder
    Public Names()
    Public Values()

    Private Sub Class_Initialize()
        ReDim Names(0)
        ReDim Values(0)
    End Sub

    Public Function Load(text)
        Dim parts, i
        parts = Split(text, ",")
        ReDim Names(UBound(parts))
        ReDim Values(UBound(parts))
        For i = 0 To UBound(parts)
            Names(i) = parts(i)
            Values(i) = parts(i)
        Next
    End Function

    Public Function Read(index)
        Read = Names(index) & ":" & Values(index)
    End Function
End Class

Dim holder
Set holder = New FieldArrayHolder
Call holder.Load("a,b")
Response.Write holder.Read(1)
%>
