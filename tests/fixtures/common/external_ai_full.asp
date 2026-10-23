<%
Set ai = Server.CreateObject("OpenASP.AI")
ai.Seed = 42

Set sequential = ai.Sequential()
Call sequential.Add(ai.Linear(2, 3))
Call sequential.Add(ai.ReLU())
Call sequential.Add(ai.Linear(3, 1))
Set input = ai.Tensor(Array(1, 2)).Reshape(Array(1, 2))
Set output = sequential.Forward(input)

Set moduleState = ai.Module("Trainable")
Set parameter = ai.Tensor(Array(1, 1)).Reshape(Array(2, 1))
parameter.RequiresGrad = True
Call moduleState.RegisterParameter("weight", parameter)
Set loss = input.MatMul(parameter)
Call loss.Backward()
Set optimizer = ai.SGD(moduleState, 0.1)
Call optimizer.Step()

Set graph = ai.Graph("AddGraph")
Set left = graph.Input("left", Array(2), "float32")
Set right = graph.Input("right", Array(2), "float32")
Set sumValue = graph.Operation(7, left, right)
Set model = graph.Build(sumValue)
Set sum = model.Forward(ai.Tensor(Array(1, 2)), ai.Tensor(Array(3, 4)))
outputShape = output.Shape

Response.Write outputShape(0) & "x" & outputShape(1) & "|" & _
               parameter.Item(0, 0) & "|" & parameter.Item(1, 0) & "|" & _
               sum.Item(0) & "|" & sum.Item(1) & "|" & moduleState.ParameterCount
%>
