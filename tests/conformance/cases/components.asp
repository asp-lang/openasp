<%
Dim dictionary, expression, matches, stream, document, root

Set dictionary = Server.CreateObject("Scripting.Dictionary")
dictionary.CompareMode = 1
dictionary.Add "Alpha", "one"

Set expression = Server.CreateObject("VBScript.RegExp")
expression.Pattern = "(a.)"
expression.Global = True
Set matches = expression.Execute("za1")

Set stream = Server.CreateObject("ADODB.Stream")
stream.Open
stream.WriteText "payload"
stream.Position = 0

Set document = Server.CreateObject("Microsoft.XMLDOM")
Set root = document.createElement("root")
root.setAttribute "id", "7"
document.appendChild root

Response.Write dictionary.Item("alpha") & "|" & matches.Item(0).Value & "|"
Response.Write stream.ReadText & "|" & document.xml
%>
