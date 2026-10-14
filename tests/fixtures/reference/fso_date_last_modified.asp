<%
Option Explicit
Dim fso, file, modified
Set fso = Server.CreateObject("Scripting.FileSystemObject")
Set file = fso.GetFile(Server.MapPath("fso_date_last_modified.asp"))
modified = file.DateLastModified
Response.Write TypeName(modified) & "|"
Response.Write Year(modified) & "-" & Month(modified) & "-" & Day(modified) & " "
Response.Write Hour(modified) & ":" & Minute(modified) & ":" & Second(modified)
%>
