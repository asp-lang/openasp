<%
' PyTorch-style CNN for MNIST.
' Conv2d(kernel=stride) is expressed as unfold/patchify plus a shared Linear.

Const ImageSize = 28
Const ClassCount = 10
Const BatchSize = 256
Const DefaultEpochCount = 3
Const LearningRate = 0.001
Const WeightDecay = 0.0001

Class Conv2d
    Private m_ai
    Private m_core
    Private m_projection
    Private m_inChannels
    Private m_outChannels
    Private m_inputSize
    Private m_kernelSize
    Private m_gridSize

    Public Sub Init(aiRuntime, inputChannels, outputChannels, inputSize, kernelSize)
        Set m_ai = aiRuntime
        m_inChannels = inputChannels
        m_outChannels = outputChannels
        m_inputSize = inputSize
        m_kernelSize = kernelSize
        m_gridSize = inputSize \ kernelSize

        Set m_core = m_ai.Module("Conv2d")
        Set m_projection = m_ai.Linear(inputChannels * kernelSize * kernelSize, outputChannels, True)
        Call m_core.RegisterModule("projection", m_projection)
    End Sub

    Public Function Forward(input, batchCount)
        ' NCHW -> N,Gh,Gw,C,Kh,Kw -> (N*Gh*Gw),(C*Kh*Kw)
        Set patches = input.Reshape(Array(batchCount, m_inChannels, m_gridSize, m_kernelSize, _
                                          m_gridSize, m_kernelSize))
        Set patches = patches.Transpose(1, 2)
        Set patches = patches.Transpose(2, 4)
        Set patches = patches.Transpose(3, 4)
        Set patches = patches.Reshape(Array(batchCount * m_gridSize * m_gridSize, _
                                            m_inChannels * m_kernelSize * m_kernelSize))

        Set projected = m_projection.Forward(patches)
        Set projected = projected.Reshape(Array(batchCount, m_gridSize, m_gridSize, m_outChannels))
        Set projected = projected.Transpose(1, 3)
        Set Forward = projected.Transpose(2, 3)
    End Function

    Public Property Get NativeModule
        Set NativeModule = m_core
    End Property
End Class

Class MnistCNN
    Private m_ai
    Private m_core
    Private m_conv1
    Private m_relu1
    Private m_conv2
    Private m_relu2
    Private m_classifier

    Public Sub Init(aiRuntime)
        Set m_ai = aiRuntime
        Set m_core = m_ai.Module("MnistCNN")

        Set m_conv1 = New Conv2d
        Call m_conv1.Init(m_ai, 1, 16, 28, 4)
        Set m_relu1 = m_ai.ReLULayer()

        Set m_conv2 = New Conv2d
        Call m_conv2.Init(m_ai, 16, 32, 7, 7)
        Set m_relu2 = m_ai.ReLULayer()
        Set m_classifier = m_ai.Linear(32, ClassCount, True)

        Call m_core.RegisterModule("conv1", m_conv1.NativeModule)
        Call m_core.RegisterModule("relu1", m_relu1)
        Call m_core.RegisterModule("conv2", m_conv2.NativeModule)
        Call m_core.RegisterModule("relu2", m_relu2)
        Call m_core.RegisterModule("classifier", m_classifier)
    End Sub

    Public Function Forward(images, batchCount)
        Set features = m_conv1.Forward(images, batchCount)
        Set features = m_relu1.Forward(features)
        Set features = m_conv2.Forward(features, batchCount)
        Set features = m_relu2.Forward(features)
        Set features = features.Reshape(Array(batchCount, 32))
        Set Forward = m_classifier.Forward(features)
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
    End Sub

    Public Sub SaveWeights(path)
        Call m_core.SaveWeights(path)
    End Sub

    Public Sub LoadWeights(path)
        Call m_core.LoadWeights(path, True)
    End Sub
End Class

dataRoot = Request.QueryString("data")
If Len(dataRoot) = 0 Then dataRoot = "data"

weightsPath = Request.QueryString("weights")
If Len(weightsPath) = 0 Then weightsPath = dataRoot & "/mnist-cnn.oaspweights"

epochCount = DefaultEpochCount
If Len(Request.QueryString("epochs")) > 0 Then epochCount = CInt(Request.QueryString("epochs"))
If epochCount < 1 Then epochCount = 1

Set ai = Server.CreateObject("OpenASP.AI")
ai.Seed = 42
ai.Deterministic = True

Set model = New MnistCNN
Call model.Init(ai)
Call model.ToDevice(ai.Device)
Set optimizer = ai.AdamW(model.NativeModule, LearningRate, WeightDecay)

Set trainSet = ai.MNIST(dataRoot & "/train-images-idx3-ubyte", dataRoot & "/train-labels-idx1-ubyte", True)
Set testSet = ai.MNIST(dataRoot & "/t10k-images-idx3-ubyte", dataRoot & "/t10k-labels-idx1-ubyte", True)
Set trainLoader = trainSet.DataLoader(BatchSize, True, 2, 4, False, True)
Set testLoader = testSet.DataLoader(BatchSize, False, 2, 4, False, True)

Response.Write "device=" & ai.Device & " train_samples=" & trainSet.Size & _
               " test_samples=" & testSet.Size & "<br>" & vbCrLf
Response.Write "model=Conv2d(1,16,k4,s4)+ReLU+Conv2d(16,32,k7,s7)+ReLU+Linear(32,10)" & _
               " optimizer=AdamW<br>" & vbCrLf

Call model.Train()
For epoch = 1 To epochCount
    Set trainIterator = trainLoader.Iterator(epoch)
    batchCount = 0
    sampleCount = 0

    Do While trainIterator.MoveNext()
        Set batch = trainIterator.Current
        Set images = batch.Inputs.Reshape(Array(batch.Size, 1, ImageSize, ImageSize)).ToDevice(ai.Device)
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

Call model.SaveWeights(weightsPath)
Response.Write "weights_saved=" & weightsPath & "<br>" & vbCrLf

Set inferenceModel = New MnistCNN
Call inferenceModel.Init(ai)
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
    Set images = batch.Inputs.Reshape(Array(batch.Size, 1, ImageSize, ImageSize)).ToDevice(ai.Device)
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
