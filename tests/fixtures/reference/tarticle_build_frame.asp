<%
Dim build_filter
build_filter = ""

Function FilterBuildHtml(ByRef html)
    Dim callbacks, i
    If build_filter = "" Then Exit Function
    callbacks = Split(build_filter, "|")
    For i = 0 To UBound(callbacks) - 1
        Execute callbacks(i) & " html"
    Next
End Function

Function FilterBuildTags(ByRef names, ByRef values)
    Dim callbacks, i
    If build_filter = "" Then Exit Function
    callbacks = Split(build_filter, "|")
    For i = 0 To UBound(callbacks) - 1
        Execute callbacks(i) & " names,values"
    Next
End Function

Class TArticleBuildProbe
    Public Html

    Private Sub Class_Initialize()
        Html = "<#title#>|<#host#>|<#tail#>"
    End Sub

    Public Function Build()
        Dim names, values, i
        Call FilterBuildHtml(Html)
        names = Array("title", "host", "tail")
        values = Array("Egret", "/asp", "done")
        Call FilterBuildTags(names, values)
        For i = 0 To UBound(names)
            Html = Replace(Html, "<#" & names(i) & "#>", values(i))
        Next
        Build = True
    End Function
End Class

Dim article, result
Set article = New TArticleBuildProbe
result = article.Build()
Response.Write CStr(result) & "|" & article.Html
%>
