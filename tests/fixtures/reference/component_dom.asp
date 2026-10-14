<%
On Error Resume Next
Set document = Server.CreateObject("MSXML2.DOMDocument")
Response.Write CStr(document.loadXML("<root><item id=""1"">one</item><item>two</item></root>"))
Response.Write ":" & CStr(Err.Number) & "|"
Set root = document.documentElement
Response.Write root.nodeName
Response.Write ":" & CStr(Err.Number) & "|"
Set first = document.selectSingleNode("//item")
Response.Write first.text
Response.Write ":" & CStr(Err.Number) & "|"
Set nodes = document.selectNodes("//item")
Response.Write nodes.length
Response.Write ":" & CStr(Err.Number) & "|"
Set parseError = document.parseError
Response.Write parseError.errorCode
Response.Write ":" & CStr(Err.Number)
%>
