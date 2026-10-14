<%
Dim value, localValue
value = Eval("40 + 2")
Execute "localValue = value + 1"
ExecuteGlobal "Function DynamicValue(): DynamicValue = localValue + 1: End Function"

On Error Resume Next
Dim ignored
ignored = 1 / 0
Response.Write value & "|" & localValue & "|" & DynamicValue() & "|" & Err.Number & "|"
Err.Clear
Set ignored = Server.CreateObject("No.Such.Component")
Response.Write Err.Number
Err.Clear
On Error GoTo 0
%>
