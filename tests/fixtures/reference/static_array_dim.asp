<%
Dim values(2), scalar
values(0) = "a"
values(1) = "b"
values(2) = "c"
scalar = "d"
Response.Write values(0) & values(1) & values(2) & "|" & UBound(values) & "|" & scalar
%>
