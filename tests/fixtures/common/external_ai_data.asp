<%
Set ai = Server.CreateObject("OpenASP.AI")
ai.Seed = 17

Set inputs = ai.Tensor(Array(1, 2, 3, 4, 5, 6, 7, 8)).Reshape(Array(4, 2))
Set labels = ai.Tensor(Array(0, 1, 0, 1))
Set dataset = ai.TensorDataset(inputs, labels)
Set loader = dataset.DataLoader(2, True, 2, 2, False, False)
Set iterator = loader.Iterator(3)
hasBatch = iterator.MoveNext()
Set batch = iterator.Current
Set batchInputs = batch.Inputs
batchShape = batchInputs.Shape

Set quantized = ai.Quantize(inputs, 4, 1, "absmax")
Set restored = ai.Dequantize(quantized, "float32")
restoredShape = restored.Shape

Set tokenizer = ai.Tokenizer("tests/fixtures/ai-vocab.txt", True)
tokens = tokenizer.Encode("hello world")
decoded = tokenizer.Decode(tokens)

Response.Write dataset.Size & "|" & hasBatch & "|" & batch.Size & "|" & _
               batchShape(0) & "|" & restoredShape(1) & "|" & _
               UBound(tokens) + 1 & "|" & decoded
%>
