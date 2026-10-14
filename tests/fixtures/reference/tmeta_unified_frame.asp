<%
Class FrameBox
    Public Value
End Class

Class TMetaFrameProbe
    Public Names()
    Public Values()

    Private Sub Class_Initialize()
        ReDim Names(0)
        ReDim Values(0)
    End Sub

    Public Function SetValue(ByRef name, ByRef value)
        Dim n, i, scratch(), helper
        name = Trim(name)
        value = value & "-stored"
        ReDim scratch(0)
        scratch(0) = name
        Set helper = New FrameBox
        helper.Value = scratch(0)

        i = 0
        For Each n In Names
            If n = name Then
                Values(i) = value
                SetValue = "updated:" & helper.Value
                Exit Function
            End If
            i = i + 1
        Next

        i = UBound(Names)
        ReDim Preserve Names(i + 1)
        ReDim Preserve Values(i + 1)
        Names(i + 1) = name
        Values(i + 1) = value
        SetValue = "added:" & helper.Value
    End Function

    Public Function ReplaceObject(ByRef target)
        Set target = New FrameBox
        target.Value = "rebound"
        ReplaceObject = target.Value
    End Function

    Public Function GrowArray(ByRef items)
        ReDim Preserve items(1)
        items(1) = "grown"
        GrowArray = UBound(items)
    End Function

    Public Function ReadValue(index)
        ReadValue = Names(index) & "=" & Values(index)
    End Function
End Class

Dim probe, key, value, result, box, items()
Set probe = New TMetaFrameProbe
key = " alpha "
value = "one"
result = probe.SetValue(key, value)
Set box = New FrameBox
box.Value = "old"
ReDim items(0)
items(0) = "kept"

Response.Write result & "|" & key & "|" & value & "|" & probe.ReadValue(1)
Response.Write "|" & probe.ReplaceObject(box) & "|" & box.Value
Response.Write "|" & probe.GrowArray(items) & "|" & items(0) & "," & items(1)
%>
