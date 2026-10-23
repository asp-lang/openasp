<%
' GRU implemented from sigmoid/tanh gates and affine modules.

Const ModelSlug = "mnist-gru"
Const ModelDescription = "GRU(input=56,hidden=64,steps=14)+Linear(64,10)"
Const SequenceLength = 14
Const InputSize = 56
Const HiddenSize = 64
Const BatchSize = 256
Const DefaultEpochCount = 3
Const LearningRate = 0.001
Const WeightDecay = 0.0001
Const GradientClipNorm = 1.0

Class MnistGRU
    Private m_ai
    Private m_core
    Private m_xz
    Private m_hz
    Private m_xr
    Private m_hr
    Private m_xn
    Private m_hn
    Private m_classifier

    Public Sub Init(ai)
        Set m_ai = ai
        Set m_core = ai.Module("MnistGRU")
        Set m_xz = ai.Linear(InputSize, HiddenSize, True)
        Set m_hz = ai.Linear(HiddenSize, HiddenSize, False)
        Set m_xr = ai.Linear(InputSize, HiddenSize, True)
        Set m_hr = ai.Linear(HiddenSize, HiddenSize, False)
        Set m_xn = ai.Linear(InputSize, HiddenSize, True)
        Set m_hn = ai.Linear(HiddenSize, HiddenSize, False)
        Set m_classifier = ai.Linear(HiddenSize, 10, True)
        Call m_core.RegisterModule("xz", m_xz)
        Call m_core.RegisterModule("hz", m_hz)
        Call m_core.RegisterModule("xr", m_xr)
        Call m_core.RegisterModule("hr", m_hr)
        Call m_core.RegisterModule("xn", m_xn)
        Call m_core.RegisterModule("hn", m_hn)
        Call m_core.RegisterModule("classifier", m_classifier)
    End Sub

    Public Function Forward(images, batchCount)
        Set sequence = images.Reshape(Array(batchCount, SequenceLength, InputSize))
        Set flattened = sequence.Reshape(Array(batchCount * SequenceLength, InputSize))
        Set updateInputs = m_xz.Forward(flattened)
        Set updateInputs = updateInputs.Reshape(Array(batchCount, SequenceLength, HiddenSize))
        Set resetInputs = m_xr.Forward(flattened)
        Set resetInputs = resetInputs.Reshape(Array(batchCount, SequenceLength, HiddenSize))
        Set candidateInputs = m_xn.Forward(flattened)
        Set candidateInputs = candidateInputs.Reshape(Array(batchCount, SequenceLength, HiddenSize))
        Set hidden = m_ai.Zeros(Array(batchCount, HiddenSize))
        Set one = m_ai.Ones(Array(batchCount, HiddenSize))
        For stepIndex = 0 To SequenceLength - 1
            Set updateInput = updateInputs.Slice(1, stepIndex, stepIndex + 1, 1)
            Set updateInput = updateInput.Reshape(Array(batchCount, HiddenSize))
            Set resetInput = resetInputs.Slice(1, stepIndex, stepIndex + 1, 1)
            Set resetInput = resetInput.Reshape(Array(batchCount, HiddenSize))
            Set candidateInput = candidateInputs.Slice(1, stepIndex, stepIndex + 1, 1)
            Set candidateInput = candidateInput.Reshape(Array(batchCount, HiddenSize))
            Set updateGate = m_ai.Sigmoid(m_ai.Add(updateInput, m_hz.Forward(hidden)))
            Set resetGate = m_ai.Sigmoid(m_ai.Add(resetInput, m_hr.Forward(hidden)))
            Set resetHidden = m_ai.Mul(resetGate, hidden)
            Set candidate = m_ai.Tanh(m_ai.Add(candidateInput, m_hn.Forward(resetHidden)))
            Set retained = m_ai.Mul(updateGate, hidden)
            Set replaced = m_ai.Mul(m_ai.Sub(one, updateGate), candidate)
            Set hidden = m_ai.Add(retained, replaced)
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
    Set instance = New MnistGRU
    Call instance.Init(ai)
    Set CreateMnistModel = instance
End Function
%>
<!--#include file="mnist-common.inc" -->
