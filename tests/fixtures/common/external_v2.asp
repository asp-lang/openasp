<%
Dim math, values, item, matrix, mapped
Set math = Server.CreateObject("Acme.Math")
Response.Write math.AsyncAdd(20, 22) & "|"
values = math.Sequence(3)
For Each item In values
    Response.Write item
Next
Response.Write "|" & math.Bytes("abc") & "|"
matrix = math.Matrix()
Response.Write matrix(1, 1) & "|"
Set mapped = math.Map()
Response.Write mapped("answer") & "|"
math.Scale = 3
Response.Write math(4) & "|"
For Each item In math
    Response.Write item
Next
Response.Write vbCrLf
%>
