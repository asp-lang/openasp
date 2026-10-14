<%
Dim i, matched
matched = 0
For i = 1 To 100000
    If CStr(i) = CStr(i) Then matched = matched + 1
Next
Response.Write matched
%>
