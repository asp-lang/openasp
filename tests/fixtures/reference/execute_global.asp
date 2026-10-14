<%
Response.ContentType = "text/plain"

ExecuteGlobal "globalValue = 17"
ExecuteGlobal "Function DynamicAdd(value)" & vbCrLf & "DynamicAdd = value + globalValue" & vbCrLf & "End Function"
ExecuteGlobal "Class DynamicThing" & vbCrLf & "Public Function Value()" & vbCrLf & "Value = 29" & vbCrLf & "End Function" & vbCrLf & "End Class"

Set dynamicObject = Eval("New DynamicThing")
Response.Write "global=" & globalValue & vbCrLf
Response.Write "routine=" & DynamicAdd(5) & vbCrLf
Response.Write "class=" & dynamicObject.Value() & vbCrLf
%>
