<%
Function LoadFromFile()
    Dim stream
    Set stream = Server.CreateObject("ADODB.Stream")
    With stream
        .Type = 2
        .Open
        LoadFromFile = .State
        .Close
    End With
End Function

Response.Write CStr(LoadFromFile())
%>
