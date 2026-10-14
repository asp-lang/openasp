<%
Dim html
html = "global"

Class Renderer
    Public html

    Public Function Build()
        html = "field"
        Build = html
    End Function
End Class

Dim renderer
Set renderer = New Renderer
Response.Write renderer.Build() & "|" & renderer.html & "|" & html
%>
