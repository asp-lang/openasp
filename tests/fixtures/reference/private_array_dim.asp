<%
Private powers(3)
powers(0) = 1
powers(1) = 2
powers(2) = 4
powers(3) = 8

Private Function ReadPower(ByVal index)
    ReadPower = powers(index)
End Function

Response.Write ReadPower(0) & ":" & ReadPower(3)
%>
