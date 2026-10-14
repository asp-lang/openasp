<%
Dim stream, dictionary, ignored
On Error Resume Next

Set stream = Server.CreateObject("ADODB.Stream")
ignored = stream.ReadText()
Response.Write Err.Number & "|"
Err.Clear

stream.Open
stream.Position = -1
Response.Write Err.Number & "|"
Err.Clear

Set dictionary = Server.CreateObject("Scripting.Dictionary")
dictionary.CompareMode = 9
Response.Write Err.Number & "|"
Err.Clear

ignored = dictionary.NoSuchMember
Response.Write Err.Number
On Error GoTo 0
%>
