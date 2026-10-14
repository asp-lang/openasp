<%
Dim i, output
output = ""
For i = 1 To 2
    Do
        If i > 0 Then
            output = output & i
            Exit Do
        End If
        output = output & "x"
    Loop
Next
Response.Write output
%>
