<%
Dim firstChunk
Dim secondChunk
Dim exhaustedChunk

firstChunk = Request.BinaryRead(102400)
secondChunk = Request.BinaryRead(102400)
exhaustedChunk = Request.BinaryRead(1)

Response.Write CStr(Len(firstChunk)) & ":" & LeftB(firstChunk, 4) & ":" & Right(firstChunk, 1)
Response.Write "|" & CStr(Len(secondChunk)) & ":" & Left(secondChunk, 1) & ":" & Right(secondChunk, 1)
Response.Write "|" & CStr(Len(exhaustedChunk)) & "|" & CStr(VarType(0)) & ":" & CStr(VarType(40000))
%>
