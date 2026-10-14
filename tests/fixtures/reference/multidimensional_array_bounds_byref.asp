<%
Dim grid(1, 2)
grid(0, 0) = "a"

Function Bounds(ByRef value)
    Bounds = LBound(value, 2) & ":" & UBound(value, 2) & ":" & value(0, 0)
End Function

Response.Write Bounds(grid) & "|" & UBound(grid, 2)
%>
