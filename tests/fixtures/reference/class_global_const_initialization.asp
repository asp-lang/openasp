<%
Const PROBE_ZERO = 0
Const PROBE_ONE = 1

Class ConstantFieldProbe
    Public ZeroValue
    Public OneValue
    Public EmptyValue

    Private Sub Class_Initialize()
        ZeroValue = PROBE_ZERO
        OneValue = PROBE_ONE
    End Sub
End Class

Dim probe
Set probe = New ConstantFieldProbe
Response.Write TypeName(probe.ZeroValue) & ":" & CStr(probe.ZeroValue) & ":" & CStr(CLng(probe.ZeroValue))
Response.Write "|" & TypeName(probe.OneValue) & ":" & CStr(probe.OneValue) & ":" & CStr(CLng(probe.OneValue))
Response.Write "|" & TypeName(probe.EmptyValue) & ":[" & CStr(probe.EmptyValue) & "]:" & CStr(CLng(probe.EmptyValue))
%>
