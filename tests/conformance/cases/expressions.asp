<%
Response.Write 2 ^ 3 & "|" & (17 \ 5) & "|" & (17 Mod 5) & "|"
Response.Write CStr(True And Not False) & "|" & CStr(True Xor False) & "|"
Response.Write CStr(True Eqv True) & "|" & CStr(False Imp False) & "|"
Response.Write "a" & 1 & ":" & (5 + 3 * 2) & ":" & (5 > 3)
%>
