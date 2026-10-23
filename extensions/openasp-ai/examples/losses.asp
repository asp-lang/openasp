<%
Set ai = Server.CreateObject("OpenASP.AI")

Set input = ai.Tensor(Array(0.2, 0.8))
Set target = ai.Tensor(Array(0.0, 1.0))
Response.Write "mse=" & ai.MSELoss(input, target, "mean").Item() & "<br>"
Response.Write "l1=" & ai.L1Loss(input, target, "mean").Item() & "<br>"
Response.Write "bce=" & ai.BCELoss(input, target, "mean").Item() & "<br>"

Set logits = ai.Tensor(Array(0.0, 2.0))
Response.Write "bce_logits=" & ai.BCEWithLogitsLoss(logits, target, "mean").Item() & "<br>"
Set positiveWeight = ai.Tensor(Array(1.0, 3.0))
Response.Write "bce_logits_weighted=" & ai.BCEWithLogitsLoss(logits, target, positiveWeight, "mean").Item() & "<br>"
Response.Write "smooth_l1=" & ai.SmoothL1Loss(input, target, 1.0, "mean").Item() & "<br>"
Response.Write "focal=" & ai.FocalLoss(logits, target, 0.25, 2.0, "mean").Item() & "<br>"
Response.Write "dice=" & ai.DiceLoss(input, target, 1.0, 0.0000001, False, "mean").Item() & "<br>"

Set logProbabilities = ai.Tensor(Array(-0.1, -2.0, -3.0, -0.2)).Reshape(Array(2, 2))
ai.DType = "int64"
Set labels = ai.Tensor(Array(0, 1))
ai.DType = "float32"
Response.Write "nll=" & ai.NLLLoss(logProbabilities, labels, "mean").Item() & "<br>"
Response.Write "cross_entropy=" & ai.CrossEntropy(logProbabilities, labels, Nothing, -100, 0.1, "mean").Item() & "<br>"

Set distributionLog = ai.Tensor(Array(-0.9162907, -0.5108256))
Set distribution = ai.Tensor(Array(0.5, 0.5))
Response.Write "kl=" & ai.KLDivLoss(distributionLog, distribution, "sum", False).Item() & "<br>"

Set huberInput = ai.Tensor(Array(0.0, 3.0))
Set zeros = ai.Tensor(Array(0.0, 0.0))
Response.Write "huber=" & ai.HuberLoss(huberInput, zeros, 1.0, "mean").Item() & "<br>"

Set features = ai.Tensor(Array(1.0, 0.0, 0.0, 1.0)).Reshape(Array(2, 2))
Response.Write "info_nce=" & ai.InfoNCELoss(features, features, 1.0, True, "mean").Item() & "<br>"

Set ctcLogProbabilities = ai.Tensor(Array(-0.5108256, -0.9162907, -0.6931472, -0.6931472)).Reshape(Array(2, 1, 2))
ai.DType = "int64"
Set ctcTargets = ai.Tensor(Array(1)).Reshape(Array(1, 1))
Set inputLengths = ai.Tensor(Array(2))
Set targetLengths = ai.Tensor(Array(1))
ai.DType = "float32"
Response.Write "ctc=" & ai.CTCLoss(ctcLogProbabilities, ctcTargets, inputLengths, targetLengths, 0, False, "mean").Item() & "<br>"
%>
