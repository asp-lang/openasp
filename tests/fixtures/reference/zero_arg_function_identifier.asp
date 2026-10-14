<%
Function DefaultLanguage()
    DefaultLanguage = 1
End Function

Function Label(languageId)
    If languageId = 1 Then
        Label = "english"
    Else
        Label = "other"
    End If
End Function

languageId = DefaultLanguage
Response.Write Label(languageId)
%>
