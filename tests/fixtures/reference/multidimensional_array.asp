<%
Dim grid(1, 2), scalar
grid(0, 0) = "a"
grid(1, 2) = "z"
Response.Write grid(0, 0) & grid(1, 2) & "|" & UBound(grid, 1) & "|" & UBound(grid, 2)
ReDim dynamicGrid(2, 1)
dynamicGrid(2, 1) = "d"
Response.Write "|" & dynamicGrid(2, 1) & "|" & UBound(dynamicGrid, 1) & "|" & UBound(dynamicGrid, 2)
%>
