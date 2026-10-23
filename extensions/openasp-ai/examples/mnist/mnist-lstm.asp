<%
' LSTM implemented from input, forget, output, and candidate gates.

Const ModelSlug = "mnist-lstm"
Const ModelDescription = "LSTM(input=56,hidden=64,steps=14)+Linear(64,10)"
Const SequenceLength = 14
Const InputSize = 56
Const HiddenSize = 64
Const BatchSize = 256
Const DefaultEpochCount = 3
Const LearningRate = 0.001
Const WeightDecay = 0.0001
Const GradientClipNorm = 1.0

Class MnistLSTM
    Private m_ai
    Private m_core
    Private m_xi
    Private m_hi
    Private m_xf
    Private m_hf
    Private m_xo
    Private m_ho
    Private m_xg
    Private m_hg
    Private m_classifier

    Public Sub Init(ai)
        Set m_ai = ai
        Set m_core = ai.Module("MnistLSTM")
        Set m_xi = ai.Linear(InputSize, HiddenSize, True)
        Set m_hi = ai.Linear(HiddenSize, HiddenSize, False)
        Set m_xf = ai.Linear(InputSize, HiddenSize, True)
        Set m_hf = ai.Linear(HiddenSize, HiddenSize, False)
        Set m_xo = ai.Linear(InputSize, HiddenSize, True)
        Set m_ho = ai.Linear(HiddenSize, HiddenSize, False)
        Set m_xg = ai.Linear(InputSize, HiddenSize, True)
        Set m_hg = ai.Linear(HiddenSize, HiddenSize, False)
        Set m_classifier = ai.Linear(HiddenSize, 10, True)
        Call m_core.RegisterModule("xi", m_xi)
        Call m_core.RegisterModule("hi", m_hi)
        Call m_core.RegisterModule("xf", m_xf)
        Call m_core.RegisterModule("hf", m_hf)
        Call m_core.RegisterModule("xo", m_xo)
        Call m_core.RegisterModule("ho", m_ho)
        Call m_core.RegisterModule("xg", m_xg)
        Call m_core.RegisterModule("hg", m_hg)
        Call m_core.RegisterModule("classifier", m_classifier)
    End Sub

    Public Function Forward(images, batchCount)
        Set sequence = images.Reshape(Array(batchCount, SequenceLength, InputSize))
        Set flattened = sequence.Reshape(Array(batchCount * SequenceLength, InputSize))
        Set inputGateInputs = m_xi.Forward(flattened)
        Set inputGateInputs = inputGateInputs.Reshape(Array(batchCount, SequenceLength, HiddenSize))
        Set forgetGateInputs = m_xf.Forward(flattened)
        Set forgetGateInputs = forgetGateInputs.Reshape(Array(batchCount, SequenceLength, HiddenSize))
        Set outputGateInputs = m_xo.Forward(flattened)
        Set outputGateInputs = outputGateInputs.Reshape(Array(batchCount, SequenceLength, HiddenSize))
        Set candidateInputs = m_xg.Forward(flattened)
        Set candidateInputs = candidateInputs.Reshape(Array(batchCount, SequenceLength, HiddenSize))
        Set hidden = m_ai.Zeros(Array(batchCount, HiddenSize))
        Set cell = m_ai.Zeros(Array(batchCount, HiddenSize))
        For stepIndex = 0 To SequenceLength - 1
            Set inputGateInput = inputGateInputs.Slice(1, stepIndex, stepIndex + 1, 1)
            Set inputGateInput = inputGateInput.Reshape(Array(batchCount, HiddenSize))
            Set forgetGateInput = forgetGateInputs.Slice(1, stepIndex, stepIndex + 1, 1)
            Set forgetGateInput = forgetGateInput.Reshape(Array(batchCount, HiddenSize))
            Set outputGateInput = outputGateInputs.Slice(1, stepIndex, stepIndex + 1, 1)
            Set outputGateInput = outputGateInput.Reshape(Array(batchCount, HiddenSize))
            Set candidateInput = candidateInputs.Slice(1, stepIndex, stepIndex + 1, 1)
            Set candidateInput = candidateInput.Reshape(Array(batchCount, HiddenSize))
            Set inputGate = m_ai.Sigmoid(m_ai.Add(inputGateInput, m_hi.Forward(hidden)))
            Set forgetGate = m_ai.Sigmoid(m_ai.Add(forgetGateInput, m_hf.Forward(hidden)))
            Set outputGate = m_ai.Sigmoid(m_ai.Add(outputGateInput, m_ho.Forward(hidden)))
            Set candidate = m_ai.Tanh(m_ai.Add(candidateInput, m_hg.Forward(hidden)))
            Set cell = m_ai.Add(m_ai.Mul(forgetGate, cell), m_ai.Mul(inputGate, candidate))
            Set hidden = m_ai.Mul(outputGate, m_ai.Tanh(cell))
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
    Set instance = New MnistLSTM
    Call instance.Init(ai)
    Set CreateMnistModel = instance
End Function
%>
<!--#include file="mnist-common.inc" -->
