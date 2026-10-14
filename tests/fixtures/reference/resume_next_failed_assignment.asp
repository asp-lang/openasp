<%
Dim value, items, dictionary, object
Class AssignmentTarget
    Public Value
End Class

Sub CheckLocal(ByRef caller)
    Dim local
    local = "local"
    On Error Resume Next
    local = CBool("")
    caller = CBool(Null)
    Response.Write local & ":" & caller & ":" & Err.Number & "|"
End Sub

Sub DisableErrors()
    On Error GoTo 0
    Exit Sub
End Sub

value = "global"
items = Array("array")
Set dictionary = Server.CreateObject("Scripting.Dictionary")
dictionary.Add "key", "dictionary"
Set object = New AssignmentTarget
object.Value = "field"
On Error Resume Next
value = CBool("")
Response.Write value & ":" & Err.Number & "|"
Err.Clear
items(0) = CBool("")
Response.Write items(0) & ":" & Err.Number & "|"
Err.Clear
dictionary("key") = CBool("")
Response.Write dictionary("key") & ":" & Err.Number & "|"
Err.Clear
object.Value = CBool("")
Response.Write object.Value & ":" & Err.Number & "|"
Err.Clear
CheckLocal value
Err.Clear
DisableErrors
Session("failed-assignment") = "session"
Session("failed-assignment") = CBool("")
Response.Write Session("failed-assignment") & ":" & Err.Number & "|"
Err.Clear
Response.ContentType = "text/plain"
Response.ContentType = CBool("")
Response.Write Err.Number
Err.Clear
%>
