<%
' Shared training, persistence, and evaluation pipeline for MNIST model examples.
' Each including file defines CreateMnistModel, ModelSlug, ModelDescription,
' BatchSize, DefaultEpochCount, LearningRate, WeightDecay, and GradientClipNorm.

dataRoot = Request.QueryString("data")
If Len(dataRoot) = 0 Then dataRoot = "data"

weightsPath = Request.QueryString("weights")
If Len(weightsPath) = 0 Then weightsPath = dataRoot & "/" & ModelSlug & ".oaspweights"

epochCount = DefaultEpochCount
If Len(Request.QueryString("epochs")) > 0 Then epochCount = CInt(Request.QueryString("epochs"))
If epochCount < 1 Then epochCount = 1

Set ai = Server.CreateObject("OpenASP.AI")
ai.Seed = 42
ai.Deterministic = True

Set model = CreateMnistModel(ai)
Call model.ToDevice(ai.Device)
Set optimizer = ai.AdamW(model.NativeModule, LearningRate, WeightDecay)

Set trainSet = ai.MNIST(dataRoot & "/train-images-idx3-ubyte", _
                        dataRoot & "/train-labels-idx1-ubyte", True)
Set testSet = ai.MNIST(dataRoot & "/t10k-images-idx3-ubyte", _
                       dataRoot & "/t10k-labels-idx1-ubyte", True)
Set trainLoader = trainSet.DataLoader(BatchSize, True, 2, 4, False, True)
Set testLoader = testSet.DataLoader(BatchSize, False, 2, 4, False, True)

Response.Write "device=" & ai.Device & _
               " train_samples=" & trainSet.Size & _
               " test_samples=" & testSet.Size & "<br>" & vbCrLf
Response.Write "model=" & ModelDescription & _
               " optimizer=AdamW learning_rate=" & LearningRate & _
               " weight_decay=" & WeightDecay & "<br>" & vbCrLf

Call model.Train()
For epoch = 1 To epochCount
    Set trainIterator = trainLoader.Iterator(epoch)
    batchCount = 0
    sampleCount = 0

    Do While trainIterator.MoveNext()
        Set batch = trainIterator.Current
        Set images = batch.Inputs.ToDevice(ai.Device)
        Set labels = batch.Labels.ToDevice(ai.Device)

        Call optimizer.ZeroGrad()
        Set logits = model.Forward(images, batch.Size)
        Set loss = ai.CrossEntropy(logits, labels)
        Call loss.Backward()
        If GradientClipNorm > 0 Then Call optimizer.ClipGradNorm(GradientClipNorm)
        Call optimizer.Step()

        batchCount = batchCount + 1
        sampleCount = sampleCount + batch.Size
    Loop

    Response.Write "epoch=" & epoch & "/" & epochCount & _
                   " samples=" & sampleCount & _
                   " batches=" & batchCount & _
                   " final_loss=" & FormatNumber(loss.Item(), 6) & "<br>" & vbCrLf
Next

Call model.SaveWeights(weightsPath)
Response.Write "weights_saved=" & weightsPath & "<br>" & vbCrLf

Set inferenceModel = CreateMnistModel(ai)
Call inferenceModel.ToDevice(ai.Device)
Call inferenceModel.LoadWeights(weightsPath)
Call inferenceModel.Eval()
Response.Write "weights_loaded=" & weightsPath & "<br>" & vbCrLf

Dim truePositive(9), falsePositive(9), falseNegative(9)
correct = 0
evaluated = 0
Set testIterator = testLoader.Iterator(0)

Do While testIterator.MoveNext()
    Set batch = testIterator.Current
    Set images = batch.Inputs.ToDevice(ai.Device)
    Set labels = batch.Labels
    Set logits = inferenceModel.Forward(images, batch.Size)
    Set probabilities = ai.Softmax(logits, 1)

    probabilityValues = probabilities.ToArray()
    labelValues = labels.ToArray()

    For sampleIndex = 0 To batch.Size - 1
        predicted = 0
        bestProbability = probabilityValues(sampleIndex * 10)
        For classIndex = 1 To 9
            probability = probabilityValues(sampleIndex * 10 + classIndex)
            If probability > bestProbability Then
                bestProbability = probability
                predicted = classIndex
            End If
        Next

        expected = CInt(labelValues(sampleIndex))
        If predicted = expected Then
            correct = correct + 1
            truePositive(expected) = truePositive(expected) + 1
        Else
            falsePositive(predicted) = falsePositive(predicted) + 1
            falseNegative(expected) = falseNegative(expected) + 1
        End If
        evaluated = evaluated + 1
    Next
Loop

If evaluated > 0 Then
    accuracy = CDbl(correct) / CDbl(evaluated)
Else
    accuracy = 0
End If

macroPrecision = 0
macroRecall = 0
Response.Write "<br>per-class metrics after reload:<br>" & vbCrLf
For classIndex = 0 To 9
    precisionDenominator = truePositive(classIndex) + falsePositive(classIndex)
    recallDenominator = truePositive(classIndex) + falseNegative(classIndex)

    If precisionDenominator > 0 Then
        precision = CDbl(truePositive(classIndex)) / CDbl(precisionDenominator)
    Else
        precision = 0
    End If
    If recallDenominator > 0 Then
        recall = CDbl(truePositive(classIndex)) / CDbl(recallDenominator)
    Else
        recall = 0
    End If

    macroPrecision = macroPrecision + precision
    macroRecall = macroRecall + recall
    Response.Write "class=" & classIndex & _
                   " precision=" & FormatNumber(precision, 4) & _
                   " recall=" & FormatNumber(recall, 4) & _
                   " tp=" & truePositive(classIndex) & _
                   " fp=" & falsePositive(classIndex) & _
                   " fn=" & falseNegative(classIndex) & "<br>" & vbCrLf
Next

macroPrecision = macroPrecision / 10
macroRecall = macroRecall / 10
Response.Write "<br>accuracy=" & FormatNumber(accuracy, 4) & _
               " macro_precision=" & FormatNumber(macroPrecision, 4) & _
               " macro_recall=" & FormatNumber(macroRecall, 4) & _
               " correct=" & correct & _
               " total=" & evaluated & "<br>" & vbCrLf
%>
