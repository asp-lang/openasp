<%
Class NestedArrayHolder
    Public Function ReadValues()
        Dim Values(100)
        Dim nested
        Values(48) = Array(100, 20, 70, 1)
        nested = Values(48)
        If IsArray(nested) Then
            ReadValues = CStr(nested(0)) & ":" & CStr(nested(1)) & ":" & CStr(UBound(nested, 1))
        Else
            ReadValues = "not-array"
        End If
    End Function
End Class

Dim holder
Set holder = New NestedArrayHolder
Response.Write holder.ReadValues()
%>
