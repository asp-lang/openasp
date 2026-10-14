<%
Class Encoder
    Public Function Encode(value)
        Encode = "[" & value & "]"
    End Function
End Class

Class IndexedValues
    Private values

    Private Sub Class_Initialize()
        Set values = Server.CreateObject("Scripting.Dictionary")
    End Sub

    Public Default Property Get Item(key)
        Item = values(key)
    End Property

    Public Default Property Let Item(key, value)
        values(key) = value
    End Property
End Class

Class NestedIndex
    Private child

    Private Sub Class_Initialize()
        Set child = New IndexedValues
    End Sub

    Public Default Property Get Item(key)
        Set Item = child
    End Property
End Class

Response.Buffer = False
Response.AddHeader "X-OpenASP-Test", "ready"

Dim joined
joined = "left" & _

    "right"

Dim encoded
Dim encoder
Set encoder = New Encoder
Response.Write encoder.Encode("assigned")
Response.Write "|"
encoded = (New Encoder).Encode("ok")
Response.Write encoded
Response.Write "|" & joined
Response.Write "|" & Request.ServerVariables("HTTP_USER_AGENT").Item
Dim nested
Set nested = New NestedIndex
nested("bucket")("answer") = "42"
Response.Write "|" & nested("bucket")("answer")
%>
