<%
Sub RedirectPermanently(location)
    Response.Clear
    Response.Status = "301 Moved Permanently"
    Response.AddHeader "Location", location
    Response.End
End Sub

Call RedirectPermanently("/next")
%>
