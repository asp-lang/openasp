<%
Class RegexpProbe
    Public Function Capture()
        Dim expression, matches, match
        Set expression = New RegExp
        expression.Pattern = "\<\#template\:(article\-([a-z0-9]*)([\-a-z0-9]*))\#\>"
        expression.IgnoreCase = True
        expression.Global = True
        Set matches = expression.Execute("<#template:article-single#>")
        For Each match In matches
            Capture = match.SubMatches(0) & "|" & match.SubMatches(1)
            Exit For
        Next
    End Function
End Class

Dim probe
Set probe = New RegexpProbe
Response.Write probe.Capture()
%>
