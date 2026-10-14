<%
Function Classify(value)
    Select Case value
        Case "alpha", "beta", "gamma"
            Classify = "first"
        Case "delta", "epsilon", "zeta"
            Classify = "second"
        Case "eta", "theta", "iota"
            Classify = "third"
        Case Else
            Classify = "other"
    End Select
End Function

Response.Write Classify("theta")
%>
