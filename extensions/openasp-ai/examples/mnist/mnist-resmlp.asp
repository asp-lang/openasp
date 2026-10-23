<%
' Residual MLP: dense feature projection followed by two residual blocks.

Const ModelSlug = "mnist-resmlp"
Const ModelDescription = "ResMLP(784,128,blocks=2,10)"
Const InputFeatures = 784
Const HiddenSize = 128
Const BatchSize = 256
Const DefaultEpochCount = 5
Const LearningRate = 0.001
Const WeightDecay = 0.0001
Const GradientClipNorm = 0.0

Class ResidualMLPBlock
    Private m_ai
    Private m_core
    Private m_fc1
    Private m_fc2

    Public Sub Init(ai)
        Set m_ai = ai
        Set m_core = ai.Module("ResidualMLPBlock")
        Set m_fc1 = ai.Linear(HiddenSize, HiddenSize, True)
        Set m_fc2 = ai.Linear(HiddenSize, HiddenSize, True)
        Call m_core.RegisterModule("fc1", m_fc1)
        Call m_core.RegisterModule("fc2", m_fc2)
    End Sub

    Public Function Forward(input)
        Set hidden = m_ai.ReLU(m_fc1.Forward(input))
        Set hidden = m_fc2.Forward(hidden)
        Set Forward = m_ai.ReLU(m_ai.Add(input, hidden))
    End Function

    Public Property Get NativeModule
        Set NativeModule = m_core
    End Property
End Class

Class MnistResMLP
    Private m_ai
    Private m_core
    Private m_input
    Private m_block1
    Private m_block2
    Private m_classifier

    Public Sub Init(ai)
        Set m_ai = ai
        Set m_core = ai.Module("MnistResMLP")
        Set m_input = ai.Linear(InputFeatures, HiddenSize, True)
        Set m_block1 = New ResidualMLPBlock
        Call m_block1.Init(ai)
        Set m_block2 = New ResidualMLPBlock
        Call m_block2.Init(ai)
        Set m_classifier = ai.Linear(HiddenSize, 10, True)
        Call m_core.RegisterModule("input", m_input)
        Call m_core.RegisterModule("block1", m_block1.NativeModule)
        Call m_core.RegisterModule("block2", m_block2.NativeModule)
        Call m_core.RegisterModule("classifier", m_classifier)
    End Sub

    Public Function Forward(images, batchCount)
        Set flattened = images.Reshape(Array(batchCount, InputFeatures))
        Set hidden = m_ai.ReLU(m_input.Forward(flattened))
        Set hidden = m_block1.Forward(hidden)
        Set hidden = m_block2.Forward(hidden)
        Set Forward = m_classifier.Forward(hidden)
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
    Set instance = New MnistResMLP
    Call instance.Init(ai)
    Set CreateMnistModel = instance
End Function
%>
<!--#include file="mnist-common.inc" -->
