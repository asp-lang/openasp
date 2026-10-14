<%
Dim parts, values
parts = Split("a,b", ",")
ReDim values(UBound(parts))
values(1) = parts(1)
Response.Write values(1)
%>
