<%
Set ai = Server.CreateObject("OpenASP.AI")
Set layer = ai.Linear(2, 1)
Set trace = ai.BeginTrace("LinearTrace")
Set symbolic = trace.Input("input", Array(-1, 2), "float32")
Set symbolicOutput = layer.Forward(symbolic)
Set model = trace.EndModel(symbolicOutput)
Set input = ai.Tensor(Array(1, 2)).Reshape(Array(1, 2))
Set output = model.Forward(input)
shape = output.Shape
Response.Write shape(0) & "x" & shape(1) & "|" & output.Item(0, 0)
%>
