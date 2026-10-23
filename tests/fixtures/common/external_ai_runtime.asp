<%
Class CheckpointModel
    Private m_native

    Public Sub Init(moduleValue)
        Set m_native = moduleValue
    End Sub

    Public Property Get NativeModule
        Set NativeModule = m_native
    End Property
End Class

Set ai = Server.CreateObject("OpenASP.AI")
Set moduleState = ai.Module("RuntimeState")
Set modelState = New CheckpointModel
Call modelState.Init(moduleState)
Set parameter = ai.Tensor(Array(2, 3))
Set running = ai.Tensor(Array(4, 5))
Call moduleState.RegisterParameter("weight", parameter)
Call moduleState.RegisterBuffer("running", running)
Set state = moduleState.StateDict()
Set parameters = moduleState.NamedParameters()
Set buffers = moduleState.Buffers()
Set cpuTensor = parameter.ToDevice("cpu:0")

devices = ai.Devices()
Set cpu = ai.GetDevice("cpu:0")
deviceCount = UBound(devices) + 1

Set optimizer = ai.AdamW(moduleState, 0.001, 0.01)
Set scheduler = ai.StepScheduler(optimizer, 10, 2, 0.5)
Set metadata = Server.CreateObject("Scripting.Dictionary")
metadata("best_loss") = 0.125
metadata("labels") = Array("cat", "dog")
checkpointPath = "/tmp/openasp-ai-component-runtime.oaspckpt"
Call ai.SaveCheckpoint(checkpointPath, modelState, optimizer, scheduler, metadata, 3, 9)
Set checkpoint = ai.LoadCheckpoint(checkpointPath, modelState, optimizer, scheduler, True)
Set restoredMetadata = checkpoint.Metadata
restoredLabels = restoredMetadata("labels")

Set graph = ai.Graph("AsyncAdd")
Set left = graph.Input("left", Array(2), "float32")
Set right = graph.Input("right", Array(2), "float32")
Set sumValue = graph.Operation(7, left, right)
Set model = graph.Build(sumValue)

Call ai.ProfilerStart(True)
Set completion = model.ForwardAsync(ai.Tensor(Array(1, 2)), ai.Tensor(Array(3, 4)))
Call completion.Wait()
Set output = completion.Output(0)
Call ai.ProfilerStop()

Response.Write state.Count & "|" & state.Name(0) & "|" & state.Kind(1) & "|" & _
               completion.Ready & "|" & completion.OutputCount & "|" & output.Item(1) & "|" & _
               parameters.Count & "|" & parameters.Name(0) & "|" & buffers.Count & "|" & _
               cpuTensor.Device & "|" & deviceCount & "|" & cpu.Backend & "|" & _
               cpu.Supports("float32") & "|" & cpu.SupportsOperator("matmul") & "|" & _
               checkpoint("epoch") & "|" & checkpoint("step") & "|" & _
               restoredMetadata("best_loss") & "|" & restoredLabels(1)
%>
