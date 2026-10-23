<%
Set ai = Server.CreateObject("OpenASP.AI")
Set sourceA = ai.Tensor(Array(1, 2, 3, 4))
Set sourceB = ai.Tensor(Array(5, 6, 7, 8))
Set a = sourceA.Reshape(Array(2, 2))
Set b = sourceB.Reshape(Array(2, 2))
Set sum = a.Add(b)
Set product = ai.MatMul(a, b)
Set zeros = ai.Zeros(Array(2, 3))
flat = product.ToArray()
shape = zeros.Shape
Response.Write sum.Item(1, 1) & "|" & product.Item(1, 0) & "|" & flat(3) & "|" & _
               zeros.Rank & "|" & zeros.Size & "|" & shape(0) & "x" & shape(1)
%>
