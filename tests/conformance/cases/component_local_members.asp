<%
Dim dictionary, keys, items, expression, matches, stream, copy, json, crypto, fso, writer, reader, path, doc, root
path = "component-local-members.txt"

Set dictionary = Server.CreateObject("Scripting.Dictionary")
dictionary.CompareMode = 1
dictionary.Add "Alpha", "one"
dictionary.Item("beta") = "two"
keys = dictionary.Keys
items = dictionary.Items
Response.Write dictionary.Exists("ALPHA") & ":" & dictionary.Count & ":" & keys(1) & ":" & items(0) & "|"
dictionary.Remove "beta"
dictionary.RemoveAll
Response.Write dictionary.Count & "|"

Set expression = Server.CreateObject("VBScript.RegExp")
expression.Pattern = "(a.)"
expression.Global = True
expression.IgnoreCase = True
Response.Write expression.Test("ZA1") & ":" & expression.Replace("za1", "x") & "|"
Set matches = expression.Execute("ZA1")
Response.Write matches.Count & ":" & matches.Item(0).Value & ":" & matches.Item(0).FirstIndex & ":" & matches.Item(0).Length & "|"

Set stream = Server.CreateObject("ADODB.Stream")
Set copy = Server.CreateObject("ADODB.Stream")
stream.Open
copy.Open
stream.WriteText "abcdef"
stream.Position = 2
stream.CopyTo copy, 2
stream.SetEOS
stream.Position = 0
copy.Position = 0
Response.Write stream.ReadText & ":" & copy.ReadText & ":" & stream.EOS & ":" & stream.State & "|"
stream.Close
copy.Close

Set json = Server.CreateObject("OpenASP.JSON")
Response.Write json.Validate("{""a"":1}") & ":" & json.Minify(" { ""a"" : 1 } ") & ":" & json.Stringify(json.Parse("{""a"":1}")) & "|"
Set crypto = Server.CreateObject("OpenASP.Crypto")
Response.Write crypto.Base64Encode("abc") & ":" & crypto.Base64Decode("YWJj") & ":" & crypto.Base64UrlEncode("??") & "|"

Set fso = Server.CreateObject("Scripting.FileSystemObject")
Set writer = fso.CreateTextFile(path, True)
writer.Write "line"
writer.WriteLine "2"
writer.Close
Response.Write fso.FileExists(path) & ":"
Set reader = fso.OpenTextFile(path)
Response.Write reader.ReadAll
reader.Close
fso.DeleteFile path
Response.Write ":" & fso.FileExists(path) & "|"

Set doc = Server.CreateObject("Microsoft.XMLDOM")
doc.loadXML "<root><child id=""7"">text</child></root>"
Set root = doc.selectSingleNode("/root/child")
root.setAttribute "name", "node"
Response.Write doc.readyState & ":" & doc.parseError.errorCode & ":" & root.nodeName & ":" & root.text & ":" & root.xml & ":" & doc.selectNodes("//child").Length
%>
