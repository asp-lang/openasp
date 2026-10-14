<%
Class IncludedRenderer
    Public html

    Public Function Build()
        Build = CStr(IsEmpty(html)) & ":" & html
        If IsEmpty(html) Then
            html = "field"
        Else
            html = "leaked:" & html
        End If
        Build = Build & "|" & html
    End Function
End Class
%>
