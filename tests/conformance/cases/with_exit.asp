<%
Class Holder
    Public Value
End Class

Class Probe
    Public Function Find()
        Dim item
        For Each item In Array("match")
            If item = "match" Then
                Find = True
                Exit Function
            End If
        Next
        Find = False
    End Function
End Class

Dim holder, probe, i, output
Set holder = New Holder
With holder
    .Value = 7
End With

output = ""
For i = 1 To 2
    Do
        output = output & i
        Exit Do
    Loop
Next

Set probe = New Probe
Call probe.Find()
Response.Write holder.Value & "|" & output & "|" & CStr(probe.Find())
%>
