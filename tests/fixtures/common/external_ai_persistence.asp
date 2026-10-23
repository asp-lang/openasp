<%
Set ai = Server.CreateObject("OpenASP.AI")
Set source = ai.Tensor(Array(1, 2, 3, 4))
Set tensor = source.Reshape(Array(2, 2))
Set Application("ai-tensor") = tensor
Set restored = Application("ai-tensor")
Response.Write restored.Item(1, 0)
%>
