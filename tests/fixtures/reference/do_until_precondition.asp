<%
Dim i, output
i = 0
output = ""
Do Until i >= 3
    output = output & i
    i = i + 1
Loop
Response.Write output
%>
