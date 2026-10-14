<%
Class Node
    Public NextNode
    Public Payload
End Class

Dim i, node
For i = 1 To 20000
    Set node = New Node
    node.Payload = String(256, "x")
    Set node.NextNode = node
Next

Response.Write "service-gc-ok"
%>
