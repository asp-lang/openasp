<%
' Vanilla RNN reads the image as a sequence of 14 two-row strips.

Const ModelSlug = "mnist-rnn"
Const ModelDescription = "RNN(input=56,hidden=64,steps=14)+Linear(64,10)"
Const SequenceLength = 14
Const InputSize = 56
Const HiddenSize = 64
Const BatchSize = 256
Const DefaultEpochCount = 3
Const LearningRate = 0.001
Const WeightDecay = 0.0001
Const GradientClipNorm = 0.0

Class MnistRNN
    Private m_ai
    Private m_core
    Private m_inputProjection
    Private m_hiddenProjection
    Private m_classifier

    Public Sub Init(ai)
        Set m_ai = ai
        Set m_core = ai.Module("MnistRNN")
        Set m_inputProjection = ai.Linear(InputSize, HiddenSize, True)
        Set m_hiddenProjection = ai.Linear(HiddenSize, HiddenSize, False)
        Set m_classifier = ai.Linear(HiddenSize, 10, True)
        Call m_core.RegisterModule("input_projection", m_inputProjection)
        Call m_core.RegisterModule("hidden_projection", m_hiddenProjection)
        Call m_core.RegisterModule("classifier", m_classifier)
    End Sub

    Public Function Forward(images, batchCount)
        Set sequence = images.Reshape(Array(batchCount, SequenceLength, InputSize))
        Set flattened = sequence.Reshape(Array(batchCount * SequenceLength, InputSize))
        Set projectedInputs = m_inputProjection.Forward(flattened)
        Set projectedInputs = projectedInputs.Reshape(Array(batchCount, SequenceLength, HiddenSize))
        Set hidden = m_ai.Zeros(Array(batchCount, HiddenSize))
        For stepIndex = 0 To SequenceLength - 1
            Set inputState = projectedInputs.Slice(1, stepIndex, stepIndex + 1, 1)
            Set inputState = inputState.Reshape(Array(batchCount, HiddenSize))
            Set recurrentState = m_hiddenProjection.Forward(hidden)
            Set hidden = m_ai.Tanh(m_ai.Add(inputState, recurrentState))
        Next
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
    Set instance = New MnistRNN
    Call instance.Init(ai)
    Set CreateMnistModel = instance
End Function
%>
<!--#include file="mnist-common.inc" -->
