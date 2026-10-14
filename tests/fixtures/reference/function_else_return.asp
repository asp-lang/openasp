<%
Function GetLanguage()
    If False Then
        GetLanguage = 2
    ElseIf False Then
        GetLanguage = 3
    Else
        GetLanguage = 1
    End If
End Function

Dim useLanguage
useLanguage = GetLanguage()
Response.Write "language=" & useLanguage & ";sql=where iLanguageID=" & useLanguage
%>
