<%
' Native callees must not overwrite an interpreted caller's local object.
' Cover statement/expression calls, properties, nested globals and Exit.
Class ScopeArticle
    Public Function LoadInfo(ByVal early)
        Dim objRS
        Set objRS = Server.CreateObject("Scripting.Dictionary")
        objRS.Add "id", 2
        If early Then
            Set objRS = Nothing
            LoadInfo = 2
            Exit Function
        End If
        Set objRS = Nothing
        LoadInfo = 2
    End Function

    Public Property Get Title
        Dim objRS
        Set objRS = Nothing
        Title = "article"
    End Property

    Public Function Nested()
        Dim objRS, value
        Set objRS = Server.CreateObject("Scripting.Dictionary")
        objRS.Add "id", 3
        value = GlobalScopeCall()
        Nested = objRS("id") + value
    End Function
End Class

Function GlobalScopeCall()
    Dim objRS
    Set objRS = Nothing
    GlobalScopeCall = 4
End Function

Function CheckComment()
    Dim objRS, article, value
    Set objRS = Server.CreateObject("Scripting.Dictionary")
    objRS.Add "id", 24
    Set article = New ScopeArticle
    article.LoadInfo False
    Response.Write objRS("id") & "|"
    value = article.LoadInfo(True)
    Response.Write objRS("id") & ":" & value & "|"
    value = article.Title
    Response.Write objRS("id") & ":" & value & "|"
    value = article.Nested()
    Response.Write objRS("id") & ":" & value
End Function

CheckComment
%>
