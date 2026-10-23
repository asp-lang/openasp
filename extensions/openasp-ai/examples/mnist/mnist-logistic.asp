<%
' Multinomial logistic regression baseline: flatten -> one affine classifier.

Const ModelSlug = "mnist-logistic"
Const ModelDescription = "SoftmaxRegression(784,10)"
Const InputFeatures = 784
Const BatchSize = 512
Const DefaultEpochCount = 10
Const LearningRate = 0.01
Const WeightDecay = 0.0001
Const GradientClipNorm = 0.0

Class MnistLogisticRegression
    Private m_core
    Private m_classifier

    Public Sub Init(ai)
        Set m_core = ai.Module("MnistLogisticRegression")
        Set m_classifier = ai.Linear(InputFeatures, 10, True)
        Call m_core.RegisterModule("classifier", m_classifier)
    End Sub

    Public Function Forward(images, batchCount)
        Set flattened = images.Reshape(Array(batchCount, InputFeatures))
        Set Forward = m_classifier.Forward(flattened)
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
    Set instance = New MnistLogisticRegression
    Call instance.Init(ai)
    Set CreateMnistModel = instance
End Function
%>
<!--#include file="mnist-common.inc" -->
