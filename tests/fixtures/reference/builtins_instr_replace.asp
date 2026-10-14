<%
Dim marker
Dim startPos
Dim endPos
Dim code
marker = "before[QS_GUESTBOOK:SAMPLE]after"
startPos = InStr(marker, "[QS_GUESTBOOK:")
endPos = InStr(startPos, marker, "]")
code = Mid(marker, startPos + 14, endPos - startPos - 14)
Response.Write "instr2=" & InStr(marker, "[QS_GUESTBOOK:") & vbCrLf
Response.Write "instr3=" & endPos & vbCrLf
Response.Write "code=" & code & vbCrLf
Response.Write "replace=" & Replace(marker, "[QS_GUESTBOOK:" & code & "]", "", 1, -1, 1) & vbCrLf
Response.Write "instrrev=" & InStrRev("a/b/c.txt", ".", -1, 1) & vbCrLf
%>
