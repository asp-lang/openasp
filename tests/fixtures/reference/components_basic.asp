<%
Set connection = Server.CreateObject("ADODB.Connection")
Set recordset = Server.CreateObject("ADODB.Recordset")
Set command = Server.CreateObject("ADODB.Command")
Set stream = Server.CreateObject("ADODB.Stream")
Set fso = Server.CreateObject("Scripting.FileSystemObject")
Set dictionary = Server.CreateObject("Scripting.Dictionary")
Set expression = Server.CreateObject("VBScript.RegExp")
Set document = Server.CreateObject("MSXML2.DOMDocument.6.0")
Set serverHttp = Server.CreateObject("MSXML2.ServerXMLHTTP.6.0")
Set clientHttp = Server.CreateObject("Microsoft.XMLHTTP")

Response.Write CStr(IsObject(connection))
Response.Write CStr(IsObject(recordset))
Response.Write CStr(IsObject(command))
Response.Write CStr(IsObject(stream))
Response.Write CStr(IsObject(fso))
Response.Write CStr(IsObject(dictionary))
Response.Write CStr(IsObject(expression))
Response.Write CStr(IsObject(document))
Response.Write CStr(IsObject(serverHttp))
Response.Write CStr(IsObject(clientHttp))
Response.Write "|"

Call stream.Open()
stream.Type = 2
stream.Charset = "utf-8"
Call stream.WriteText("hello")
stream.Position = 0
Response.Write stream.ReadText()
Response.Write "|"

tempPath = Request("tmp")
Set writer = fso.CreateTextFile(tempPath, True)
Call writer.WriteLine("line-one")
Call writer.Write("line-two")
Call writer.Close()
Response.Write CStr(fso.FileExists(tempPath))
Response.Write ":"
Set reader = fso.OpenTextFile(tempPath, 1)
Response.Write reader.ReadAll()
Call reader.Close()
Call fso.DeleteFile(tempPath)
Response.Write "|"

expression.Pattern = "[a-z]+"
expression.Global = True
expression.IgnoreCase = True
Set matches = expression.Execute("One 22 TWO")
Response.Write CStr(expression.Test("123 abc"))
Response.Write ":"
Response.Write matches.Count
Response.Write ":"
Response.Write matches(0).Value
Response.Write ":"
Response.Write expression.Replace("One TWO", "x")
Response.Write "|"

Response.Write CStr(document.loadXML("<root><item id=""1"">one</item><item>two</item></root>"))
Response.Write ":"
Set documentRoot = document.documentElement
Response.Write documentRoot.nodeName
Response.Write ":"
Set firstItem = document.selectSingleNode("//item")
Response.Write firstItem.text
Response.Write ":"
Set itemNodes = document.selectNodes("//item")
Response.Write itemNodes.length
Response.Write ":"
Set documentError = document.parseError
Response.Write documentError.errorCode
%>
