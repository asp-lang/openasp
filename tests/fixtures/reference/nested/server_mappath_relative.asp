<%
Dim fso,p
Set fso=Server.CreateObject("Scripting.FileSystemObject")
p=Server.MapPath(".") & "\"
Response.Write p & "|" & fso.FileExists(p & "SERVER_MAPPATH_RELATIVE.ASP") & "|" & fso.FolderExists(p & "..\")
If fso.FolderExists(p & "..\") Then p=p & "..\"
Response.Write "|" & fso.GetFolder(p).Path
%>
