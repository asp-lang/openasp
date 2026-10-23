<%
' LeNet-style network using non-overlapping learned 2x2 patch convolutions.

Const ModelSlug = "mnist-lenet"
Const ModelDescription = "LeNetStyle(1x28x28,Conv6,Conv16,120,84,10)"
Const BatchSize = 256
Const DefaultEpochCount = 3
Const LearningRate = 0.001
Const WeightDecay = 0.0001
Const GradientClipNorm = 0.0

Class PatchConv2d
    Private m_projection
    Private m_core
    Private m_inChannels
    Private m_outChannels
    Private m_kernelSize
    Private m_gridSize

    Public Sub Init(ai, inputChannels, outputChannels, inputSize, kernelSize)
        m_inChannels = inputChannels
        m_outChannels = outputChannels
        m_kernelSize = kernelSize
        m_gridSize = inputSize \ kernelSize
        Set m_core = ai.Module("PatchConv2d")
        Set m_projection = ai.Linear(inputChannels * kernelSize * kernelSize, outputChannels, True)
        Call m_core.RegisterModule("projection", m_projection)
    End Sub

    Public Function Forward(input, batchCount)
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

Class MnistLeNet
    Private m_ai
    Private m_core
    Private m_conv1
    Private m_conv2
    Private m_fc1
    Private m_fc2
    Private m_classifier

    Public Sub Init(ai)
        Set m_ai = ai
        Set m_core = ai.Module("MnistLeNet")
        Set m_conv1 = New PatchConv2d
        Call m_conv1.Init(ai, 1, 6, 28, 2)
        Set m_conv2 = New PatchConv2d
        Call m_conv2.Init(ai, 6, 16, 14, 2)
        Set m_fc1 = ai.Linear(16 * 7 * 7, 120, True)
        Set m_fc2 = ai.Linear(120, 84, True)
        Set m_classifier = ai.Linear(84, 10, True)
        Call m_core.RegisterModule("conv1", m_conv1.NativeModule)
        Call m_core.RegisterModule("conv2", m_conv2.NativeModule)
        Call m_core.RegisterModule("fc1", m_fc1)
        Call m_core.RegisterModule("fc2", m_fc2)
        Call m_core.RegisterModule("classifier", m_classifier)
    End Sub

    Public Function Forward(images, batchCount)
        Set features = images.Reshape(Array(batchCount, 1, 28, 28))
        Set features = m_ai.ReLU(m_conv1.Forward(features, batchCount))
        Set features = m_ai.ReLU(m_conv2.Forward(features, batchCount))
        Set features = features.Reshape(Array(batchCount, 16 * 7 * 7))
        Set features = m_ai.ReLU(m_fc1.Forward(features))
        Set features = m_ai.ReLU(m_fc2.Forward(features))
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

Function CreateMnistModel(ai)
    Set instance = New MnistLeNet
    Call instance.Init(ai)
    Set CreateMnistModel = instance
End Function
%>
<!--#include file="mnist-common.inc" -->
