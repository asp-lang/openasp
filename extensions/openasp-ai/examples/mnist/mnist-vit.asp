<%
' PyTorch-style Vision Transformer for MNIST.
' Patchify: [batch, 28, 28] -> [batch, 7, 4, 7, 4]
'        -> [batch, 7, 7, 4, 4] -> [batch, 49, 16].

Const ImageSize = 28
Const PatchSize = 4
Const PatchGridSize = 7
Const PatchCount = 49
Const PatchDimension = 16
Const HiddenSize = 32
Const FeedForwardSize = 64
Const ClassCount = 10
Const BatchSize = 256
Const DefaultEpochCount = 1
Const LearningRate = 0.001
Const WeightDecay = 0.0001

Class VisionTransformerEncoderBlock
    Private m_ai
    Private m_core
    Private m_query
    Private m_key
    Private m_value
    Private m_attentionOutput
    Private m_norm1
    Private m_norm2
    Private m_feedForward1
    Private m_feedForward2

    Public Sub Init(aiRuntime)
        Set m_ai = aiRuntime
        Set m_core = m_ai.Module("VisionTransformerEncoderBlock")
        Set m_query = m_ai.Linear(HiddenSize, HiddenSize, True)
        Set m_key = m_ai.Linear(HiddenSize, HiddenSize, True)
        Set m_value = m_ai.Linear(HiddenSize, HiddenSize, True)
        Set m_attentionOutput = m_ai.Linear(HiddenSize, HiddenSize, True)
        Set m_norm1 = m_ai.LayerNormLayer(HiddenSize)
        Set m_norm2 = m_ai.LayerNormLayer(HiddenSize)
        Set m_feedForward1 = m_ai.Linear(HiddenSize, FeedForwardSize, True)
        Set m_feedForward2 = m_ai.Linear(FeedForwardSize, HiddenSize, True)

        Call m_core.RegisterModule("query", m_query)
        Call m_core.RegisterModule("key", m_key)
        Call m_core.RegisterModule("value", m_value)
        Call m_core.RegisterModule("attention_output", m_attentionOutput)
        Call m_core.RegisterModule("norm1", m_norm1)
        Call m_core.RegisterModule("norm2", m_norm2)
        Call m_core.RegisterModule("feed_forward1", m_feedForward1)
        Call m_core.RegisterModule("feed_forward2", m_feedForward2)
    End Sub

    Public Function Forward(tokens, batchCount)
        Set normalized = m_norm1.Forward(tokens)
        Set flat = normalized.Reshape(Array(batchCount * PatchCount, HiddenSize))

        Set query = m_query.Forward(flat).Reshape(Array(batchCount, PatchCount, HiddenSize))
        Set key = m_key.Forward(flat).Reshape(Array(batchCount, PatchCount, HiddenSize))
        Set value = m_value.Forward(flat).Reshape(Array(batchCount, PatchCount, HiddenSize))
        Set attended = m_ai.ScaledDotProductAttention(query, key, value)

        Set attendedFlat = attended.Reshape(Array(batchCount * PatchCount, HiddenSize))
        Set projected = m_attentionOutput.Forward(attendedFlat)
        Set projected = projected.Reshape(Array(batchCount, PatchCount, HiddenSize))
        Set residual = m_ai.Add(tokens, projected)

        Set normalized = m_norm2.Forward(residual)
        Set flat = normalized.Reshape(Array(batchCount * PatchCount, HiddenSize))
        Set hidden = m_feedForward1.Forward(flat)
        Set hidden = m_ai.GELU(hidden)
        Set hidden = m_feedForward2.Forward(hidden)
        Set hidden = hidden.Reshape(Array(batchCount, PatchCount, HiddenSize))
        Set Forward = m_ai.Add(residual, hidden)
    End Function

    Public Property Get NativeModule
        Set NativeModule = m_core
    End Property
End Class

Class MnistViT
    Private m_ai
    Private m_core
    Private m_patchEmbedding
    Private m_positionEncoding
    Private m_encoder1
    Private m_finalNorm
    Private m_classifier

    Public Sub Init(aiRuntime)
        Set m_ai = aiRuntime
        Set m_core = m_ai.Module("MnistViT")
        Set m_patchEmbedding = m_ai.Linear(PatchDimension, HiddenSize, True)
        Set m_encoder1 = New VisionTransformerEncoderBlock
        Call m_encoder1.Init(m_ai)
        Set m_finalNorm = m_ai.LayerNormLayer(HiddenSize)
        Set m_classifier = m_ai.Linear(HiddenSize, ClassCount, True)

        ReDim positionValues(PatchCount * HiddenSize - 1)
        For tokenIndex = 0 To PatchCount - 1
            For channelIndex = 0 To HiddenSize - 1
                frequency = tokenIndex / (10000 ^ (2 * Int(channelIndex / 2) / HiddenSize))
                If channelIndex Mod 2 = 0 Then
                    positionValues(tokenIndex * HiddenSize + channelIndex) = 0.1 * Sin(frequency)
                Else
                    positionValues(tokenIndex * HiddenSize + channelIndex) = 0.1 * Cos(frequency)
                End If
            Next
        Next
        Set m_positionEncoding = m_ai.Tensor(positionValues).Reshape(Array(1, PatchCount, HiddenSize))

        Call m_core.RegisterModule("patch_embedding", m_patchEmbedding)
        Call m_core.RegisterModule("encoder1", m_encoder1.NativeModule)
        Call m_core.RegisterModule("final_norm", m_finalNorm)
        Call m_core.RegisterModule("classifier", m_classifier)
        Call m_core.RegisterBuffer("position_encoding", m_positionEncoding)
    End Sub

    Public Function Forward(images, batchCount)
        Set patches = images.Reshape(Array(batchCount, PatchGridSize, PatchSize, PatchGridSize, PatchSize))
        Set patches = patches.Transpose(2, 3)
        Set patches = patches.Reshape(Array(batchCount * PatchCount, PatchDimension))

        Set tokens = m_patchEmbedding.Forward(patches)
        Set tokens = tokens.Reshape(Array(batchCount, PatchCount, HiddenSize))
        Set tokens = m_ai.Add(tokens, m_positionEncoding)
        Set tokens = m_encoder1.Forward(tokens, batchCount)
        Set tokens = m_finalNorm.Forward(tokens)
        Set pooled = m_ai.Mean(tokens, 1, False)
        Set Forward = m_classifier.Forward(pooled)
    End Function

    Public Property Get NativeModule
        Set NativeModule = m_core
    End Property

    Public Sub Train()
        Call m_core.Train()
    End Sub

    Public Sub Eval()
        Call m_core.Eval()
    End Sub

    Public Sub ToDevice(deviceName)
        Call m_core.ToDevice(deviceName)
        Set m_positionEncoding = m_positionEncoding.ToDevice(deviceName)
    End Sub

    Public Sub SaveWeights(path)
        Call m_core.SaveWeights(path)
    End Sub

    Public Sub LoadWeights(path)
        Call m_core.LoadWeights(path, True)
    End Sub

    ' #region debug-point E:position-encoding-values
    Public Function DebugPositionEncodingArray()
        DebugPositionEncodingArray = m_positionEncoding.ToArray()
    End Function
    ' #endregion
End Class

dataRoot = Request.QueryString("data")
If Len(dataRoot) = 0 Then dataRoot = "data"

weightsPath = Request.QueryString("weights")
If Len(weightsPath) = 0 Then weightsPath = dataRoot & "/mnist-vit.oaspweights"

epochCount = DefaultEpochCount
If Len(Request.QueryString("epochs")) > 0 Then epochCount = CInt(Request.QueryString("epochs"))
If epochCount < 1 Then epochCount = 1

' #region debug-point C-E:accuracy-probes
debugAccuracy = Request.QueryString("debug_accuracy") = "1"
' #endregion

Set ai = Server.CreateObject("OpenASP.AI")
ai.Seed = 42
ai.Deterministic = True

Set model = New MnistViT
Call model.Init(ai)
Call model.ToDevice(ai.Device)

' #region debug-point E:position-encoding-range
If debugAccuracy Then
    positionEncodingValues = model.DebugPositionEncodingArray()
    positionMinimum = CDbl(positionEncodingValues(0))
    positionMaximum = positionMinimum
    For debugIndex = 1 To UBound(positionEncodingValues)
        positionValue = CDbl(positionEncodingValues(debugIndex))
        If positionValue < positionMinimum Then positionMinimum = positionValue
        If positionValue > positionMaximum Then positionMaximum = positionValue
    Next
    Response.Write "debug_position_shape=1x49x32" & _
                   " min=" & positionMinimum & _
                   " max=" & positionMaximum & "<br>" & vbCrLf
End If
' #endregion

Set optimizer = ai.AdamW(model.NativeModule, LearningRate, WeightDecay)

Set trainSet = ai.MNIST(dataRoot & "/train-images-idx3-ubyte", dataRoot & "/train-labels-idx1-ubyte", True)
Set testSet = ai.MNIST(dataRoot & "/t10k-images-idx3-ubyte", dataRoot & "/t10k-labels-idx1-ubyte", True)
Set trainLoader = trainSet.DataLoader(BatchSize, True, 2, 4, False, True)
Set testLoader = testSet.DataLoader(BatchSize, False, 2, 4, False, True)

Response.Write "device=" & ai.Device & " train_samples=" & trainSet.Size & _
               " test_samples=" & testSet.Size & "<br>" & vbCrLf
Response.Write "model=Class MnistViT(patch=4x4,tokens=49,hidden=32,encoders=1,ffn=64)+SoftMax optimizer=AdamW<br>" & vbCrLf

Call model.Train()
For epoch = 1 To epochCount
    Set trainIterator = trainLoader.Iterator(epoch)
    batchCount = 0
    sampleCount = 0

    Do While trainIterator.MoveNext()
        Set batch = trainIterator.Current
        Set images = batch.Inputs.Reshape(Array(batch.Size, ImageSize, ImageSize)).ToDevice(ai.Device)
        Set labels = batch.Labels.ToDevice(ai.Device)

        Call optimizer.ZeroGrad()
        Set logits = model.Forward(images, batch.Size)
        Set loss = ai.CrossEntropy(logits, labels)
        Call loss.Backward()
        Call optimizer.Step()

        batchCount = batchCount + 1
        sampleCount = sampleCount + batch.Size
    Loop

    Response.Write "epoch=" & epoch & "/" & epochCount & _
                   " samples=" & sampleCount & _
                   " batches=" & batchCount & _
                   " final_loss=" & FormatNumber(loss.Item(), 6) & "<br>" & vbCrLf
Next

' #region debug-point C:pre-save-logits
If debugAccuracy Then
    Call model.Eval()
    Set debugIterator = testLoader.Iterator(0)
    If debugIterator.MoveNext() Then
        Set debugBatch = debugIterator.Current
        Set debugImages = debugBatch.Inputs.Reshape(Array(debugBatch.Size, ImageSize, ImageSize)).ToDevice(ai.Device)
        Set debugPreLogits = model.Forward(debugImages, debugBatch.Size)
        debugPreLogitValues = debugPreLogits.ToArray()
    End If
End If
' #endregion

Call model.SaveWeights(weightsPath)
Response.Write "weights_saved=" & weightsPath & "<br>" & vbCrLf

Set inferenceModel = New MnistViT
Call inferenceModel.Init(ai)
Call inferenceModel.ToDevice(ai.Device)
Call inferenceModel.LoadWeights(weightsPath)
Call inferenceModel.Eval()
Response.Write "weights_loaded=" & weightsPath & "<br>" & vbCrLf

' #region debug-point C:post-load-logits
If debugAccuracy Then
    Set debugPostLogits = inferenceModel.Forward(debugImages, debugBatch.Size)
    debugPostLogitValues = debugPostLogits.ToArray()
    debugReloadMaxAbs = 0
    For debugIndex = 0 To UBound(debugPreLogitValues)
        debugDifference = Abs(CDbl(debugPreLogitValues(debugIndex)) - CDbl(debugPostLogitValues(debugIndex)))
        If debugDifference > debugReloadMaxAbs Then debugReloadMaxAbs = debugDifference
    Next
    Response.Write "debug_reload_logits=" & CStr(UBound(debugPreLogitValues) + 1) & _
                   " max_abs=" & CStr(debugReloadMaxAbs) & "<br>" & vbCrLf
End If
' #endregion

Dim truePositive(9), falsePositive(9), falseNegative(9)
correct = 0
evaluated = 0
Set testIterator = testLoader.Iterator(0)

Do While testIterator.MoveNext()
    Set batch = testIterator.Current
    Set images = batch.Inputs.Reshape(Array(batch.Size, ImageSize, ImageSize)).ToDevice(ai.Device)
    Set labels = batch.Labels
    Set logits = inferenceModel.Forward(images, batch.Size)
    Set probabilities = ai.Softmax(logits, 1)

    probabilityValues = probabilities.ToArray()
    labelValues = labels.ToArray()

    For sampleIndex = 0 To batch.Size - 1
        predicted = 0
        bestProbability = probabilityValues(sampleIndex * ClassCount)
        For classIndex = 1 To ClassCount - 1
            probability = probabilityValues(sampleIndex * ClassCount + classIndex)
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
For classIndex = 0 To ClassCount - 1
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

macroPrecision = macroPrecision / ClassCount
macroRecall = macroRecall / ClassCount
Response.Write "<br>accuracy=" & FormatNumber(accuracy, 4) & _
               " macro_precision=" & FormatNumber(macroPrecision, 4) & _
               " macro_recall=" & FormatNumber(macroRecall, 4) & _
               " correct=" & correct & _
               " total=" & evaluated & "<br>" & vbCrLf
%>
