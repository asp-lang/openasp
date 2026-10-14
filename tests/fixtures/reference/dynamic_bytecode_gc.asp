<%
Response.ContentType = "text/plain"

definitionSource = "Sub DynamicBump(ByRef value)" & vbCrLf _
    & "value = value + 1" & vbCrLf _
    & "End Sub" & vbCrLf _
    & "Class DynamicBox" & vbCrLf _
    & "Private stored" & vbCrLf _
    & "Public Property Let Value(nextValue)" & vbCrLf _
    & "stored = nextValue" & vbCrLf _
    & "End Property" & vbCrLf _
    & "Public Property Get Value()" & vbCrLf _
    & "Value = stored" & vbCrLf _
    & "End Property" & vbCrLf _
    & "End Class"

ExecuteGlobal definitionSource
tempValue = 0
Execute "tempValue = tempValue + 1"
Execute "tempValue = tempValue + 1"
firstEval = Eval("20 + 2")
secondEval = Eval("20 + 2")

counter = 9
Call DynamicBump(counter)
Set box = New DynamicBox
box.Value = 31

Response.Write "counter=" & counter & vbCrLf
Response.Write "object=" & box.Value & vbCrLf
Response.Write "execute=" & tempValue & vbCrLf
Response.Write "eval=" & firstEval + secondEval & vbCrLf
%>
