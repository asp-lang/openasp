<%
Dim Action_Plugin_GetRights_Begin, bridge_seen, requestedAction, result

Action_Plugin_GetRights_Begin = Array("BridgePlugin action, localValue")
bridge_seen = ""

Sub BridgePlugin(ByRef action, ByRef localValue)
    action = action & "-plugin"
    localValue = localValue & "-changed"
    bridge_seen = action & "|" & localValue
End Sub

Function ExecuteFrameBridgeFixture(ByRef action)
    Dim callback, localValue
    localValue = "local"
    For Each callback In Action_Plugin_GetRights_Begin
        If Not IsEmpty(callback) Then Call Execute(callback)
    Next
    ExecuteFrameBridgeFixture = action & "|" & localValue
End Function

requestedAction = "read"
result = ExecuteFrameBridgeFixture(requestedAction)
Response.Write result & "|" & requestedAction & "|" & bridge_seen
%>
