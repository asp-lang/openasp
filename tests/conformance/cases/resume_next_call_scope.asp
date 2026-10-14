<%
Function SuppressedFailure()
    On Error Resume Next
    SuppressedFailure = Left("value", -1)
End Function

Sub SuccessfulCall()
End Sub

On Error Resume Next
Response.Write Err.Number & "|"
Call SuppressedFailure()
Response.Write Err.Number & "|"
ignored = Left("value", -1)
Call SuccessfulCall()
Response.Write Err.Number & "|"
Err.Clear
ignored = SuppressedFailure()
Response.Write Err.Number & "|"
Response.Write "continued"
%>
