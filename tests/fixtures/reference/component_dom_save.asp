<%
On Error Resume Next
Set document = Server.CreateObject("MSXML2.DOMDocument")
Call document.loadXML("<root><item>saved</item></root>")
Call document.save(Request("file"))
If Err.Number <> 0 Then
    Response.Write "save-error:" & Err.Number & ":" & Err.Description
    Response.End
End If

Set loaded = Server.CreateObject("MSXML2.DOMDocument")
Response.Write CStr(loaded.load(Request("file"))) & "|"
If Err.Number <> 0 Then
    Response.Write "load-error:" & Err.Number & ":" & Err.Description
Else
    Response.Write loaded.selectSingleNode("//item").text
End If
%>
