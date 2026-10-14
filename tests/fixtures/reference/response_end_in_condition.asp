<%
Sub StopRequest()
    Response.Write "before"
    Response.End
End Sub

Class UploadProbe
    Public Function Save()
        Call StopRequest()
        Save=True
    End Function
End Class

Function UploadFile()
    Dim probe
    Set probe=New UploadProbe
    If probe.Save() Then UploadFile=True
End Function

If UploadFile() Then
    Response.Write "then"
Else
    Response.Clear
    Response.Write "else"
End If

Response.Write "after"
%>
