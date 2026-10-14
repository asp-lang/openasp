<%
Dim fso, writer, appender, reader, path
path = "fso-open-text-modes.txt"
Set fso = Server.CreateObject("Scripting.FileSystemObject")

Set writer = fso.OpenTextFile(path, 2, True)
writer.Write "first"
writer.Close

Set appender = fso.OpenTextFile(path, 8, True)
appender.WriteLine "-second"
appender.Close

Set reader = fso.OpenTextFile(path, 1)
Response.Write reader.ReadAll
reader.Close
fso.DeleteFile path
%>
