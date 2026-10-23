<%
' MLP-Mixer alternates token-mixing and channel-mixing residual MLPs.

Const ModelSlug = "mnist-mixer"
Const ModelDescription = "MLP-Mixer(patch=4,tokens=49,hidden=64,blocks=2)"
Const ImageSize = 28
Const PatchSize = 4
Const GridSize = 7
Const PatchCount = 49
Const PatchDimension = 16
Const HiddenSize = 64
Const TokenMLPSize = 32
Const ChannelMLPSize = 128
Const BatchSize = 256
Const DefaultEpochCount = 3
Const LearningRate = 0.001
Const WeightDecay = 0.0001
Const GradientClipNorm = 0.0

Class MixerBlock
    Private m_ai
    Private m_core
    Private m_norm1
    Private m_token1
    Private m_token2
    Private m_norm2
    Private m_channel1
    Private m_channel2

    Public Sub Init(ai)
        Set m_ai = ai
        Set m_core = ai.Module("MixerBlock")
        Set m_norm1 = ai.LayerNormLayer(HiddenSize)
        Set m_token1 = ai.Linear(PatchCount, TokenMLPSize, True)
        Set m_token2 = ai.Linear(TokenMLPSize, PatchCount, True)
        Set m_norm2 = ai.LayerNormLayer(HiddenSize)
        Set m_channel1 = ai.Linear(HiddenSize, ChannelMLPSize, True)
        Set m_channel2 = ai.Linear(ChannelMLPSize, HiddenSize, True)
        Call m_core.RegisterModule("norm1", m_norm1)
        Call m_core.RegisterModule("token1", m_token1)
        Call m_core.RegisterModule("token2", m_token2)
        Call m_core.RegisterModule("norm2", m_norm2)
        Call m_core.RegisterModule("channel1", m_channel1)
        Call m_core.RegisterModule("channel2", m_channel2)
    End Sub

    Public Function Forward(tokens, batchCount)
        Set normalized = m_norm1.Forward(tokens)
        Set tokenChannels = normalized.Transpose(1, 2)
        Set tokenChannels = tokenChannels.Reshape(Array(batchCount * HiddenSize, PatchCount))
        Set mixedTokens = m_token1.Forward(tokenChannels)
        Set mixedTokens = m_ai.GELU(mixedTokens)
        Set mixedTokens = m_token2.Forward(mixedTokens)
        Set mixedTokens = mixedTokens.Reshape(Array(batchCount, HiddenSize, PatchCount))
        Set mixedTokens = mixedTokens.Transpose(1, 2)
        Set tokens = m_ai.Add(tokens, mixedTokens)

        Set normalized = m_norm2.Forward(tokens)
        Set channels = normalized.Reshape(Array(batchCount * PatchCount, HiddenSize))
        Set channels = m_channel1.Forward(channels)
        Set channels = m_ai.GELU(channels)
        Set channels = m_channel2.Forward(channels)
        Set channels = channels.Reshape(Array(batchCount, PatchCount, HiddenSize))
        Set Forward = m_ai.Add(tokens, channels)
    End Function

    Public Property Get NativeModule
        Set NativeModule = m_core
    End Property
End Class

Class MnistMixer
    Private m_ai
    Private m_core
    Private m_patchEmbedding
    Private m_block1
    Private m_block2
    Private m_norm
    Private m_classifier

    Public Sub Init(ai)
        Set m_ai = ai
        Set m_core = ai.Module("MnistMixer")
        Set m_patchEmbedding = ai.Linear(PatchDimension, HiddenSize, True)
        Set m_block1 = New MixerBlock
        Call m_block1.Init(ai)
        Set m_block2 = New MixerBlock
        Call m_block2.Init(ai)
        Set m_norm = ai.LayerNormLayer(HiddenSize)
        Set m_classifier = ai.Linear(HiddenSize, 10, True)
        Call m_core.RegisterModule("patch_embedding", m_patchEmbedding)
        Call m_core.RegisterModule("block1", m_block1.NativeModule)
        Call m_core.RegisterModule("block2", m_block2.NativeModule)
        Call m_core.RegisterModule("norm", m_norm)
        Call m_core.RegisterModule("classifier", m_classifier)
    End Sub

    Public Function Forward(images, batchCount)
        Set patches = images.Reshape(Array(batchCount, GridSize, PatchSize, GridSize, PatchSize))
        Set patches = patches.Transpose(2, 3)
        Set patches = patches.Reshape(Array(batchCount * PatchCount, PatchDimension))
        Set tokens = m_patchEmbedding.Forward(patches)
        Set tokens = tokens.Reshape(Array(batchCount, PatchCount, HiddenSize))
        Set tokens = m_block1.Forward(tokens, batchCount)
        Set tokens = m_block2.Forward(tokens, batchCount)
        Set tokens = m_norm.Forward(tokens)
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
    End Sub

    Public Sub SaveWeights(path)
        Call m_core.SaveWeights(path)
    End Sub

    Public Sub LoadWeights(path)
        Call m_core.LoadWeights(path, True)
    End Sub
End Class

Function CreateMnistModel(ai)
    Set instance = New MnistMixer
    Call instance.Init(ai)
    Set CreateMnistModel = instance
End Function
%>
<!--#include file="mnist-common.inc" -->
